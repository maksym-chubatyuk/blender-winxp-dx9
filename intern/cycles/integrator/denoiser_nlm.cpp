/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/denoiser_nlm.h"

#include <algorithm>
#include <cstring>

#include "integrator/nlm/nlm_cpu.h"
#include "integrator/nlm/prefilter.h"
#include "integrator/nlm/reconstruction.h"
#include "integrator/nlm/transform.h"

#include "session/buffers.h"

#include "util/log.h"
#include "util/rect.h"
#include "util/time.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

namespace {

/* The defaults of the settings the NLM denoiser had in Blender 2.9x. */
constexpr int NLM_RADIUS = 8;
constexpr float NLM_STRENGTH = 0.5f;
constexpr float NLM_FEATURE_STRENGTH = 0.5f;

/* Denoised in tiles of this size, with a margin of the radius: the regression stores ~200 floats
 * per pixel. */
constexpr int NLM_TILE_SIZE = 128;

/* Passes of the denoising buffer of a tile (#NLMTile::buffer). */
enum {
  NLM_BUFFER_DEPTH = 0,
  NLM_BUFFER_NORMAL = 1,
  NLM_BUFFER_SHADOWING = 4,
  NLM_BUFFER_ALBEDO = 5,
  NLM_BUFFER_COLOR = 8,
  NLM_BUFFER_COLOR_VARIANCE = 11,
  NLM_BUFFER_NUM_PASSES = 14,
};

/* Floats for the 4-wide loads and stores of the filters: aligned to 16 bytes, with a little space
 * before and after since the filters read slightly outside of the images. */
class FilterMemory {
 public:
  void alloc(const size_t size)
  {
    data_.clear();
    data_.resize(size + 8, 0.0f);
  }

  float *get(const size_t offset = 0)
  {
    return data_.data() + 4 + offset;
  }

 private:
  vector<float> data_;
};

/* Read access to the passes of the render buffer, in coordinates of the denoised window. */
struct NLMInput {
  const float *buffer = nullptr;
  int64_t offset = 0;
  int64_t stride = 0;
  int64_t pass_stride = 0;
  int num_samples = 0;

  int combined = PASS_UNUSED;
  int normal = PASS_UNUSED;
  int albedo = PASS_UNUSED;
  int data = PASS_UNUSED;
  int sample_count = PASS_UNUSED;

  const float *pixel(const int x, const int y) const
  {
    return buffer + (offset + x + y * stride) * pass_stride;
  }

  int samples(const float *pixel) const
  {
    const int samples = (sample_count != PASS_UNUSED) ? int(__float_as_uint(pixel[sample_count])) :
                                                         num_samples;
    return max(samples, 1);
  }
};

/* Mean of a pass and the variance of the mean, from its sums and sums of squares over the samples
 * (like `kernel_filter_get_feature` did). */
void mean_and_variance(
    const float sum, const float sum_squared, const int n, float *mean, float *variance)
{
  *mean = sum / n;
  /* Can't compute variance with single sample, just set it very high. */
  *variance = (n > 1) ? max(0.0f, (sum_squared - sum * (*mean)) / (float(n) * (n - 1))) : 1e10f;
}

struct NLMTile {
  /* Denoised pixels: x, y, width, height. */
  int4 filter_area;
  /* Pixels read for them, the filter area expanded by the radius: x0, y0, x1, y1 (exclusive). */
  int4 rect;
  int stride = 0;
  int pass_stride = 0;

  FilterMemory buffer;
  /* Two images per shift and one for the weights. */
  FilterMemory temporary;

  float *pass(const int pass)
  {
    return buffer.get(size_t(pass) * pass_stride);
  }
};

void non_local_means(NLMTile &tile,
                     const float *image,
                     const float *guide,
                     const float *variance,
                     float *out,
                     const int r,
                     const int f,
                     const float a,
                     const float k_2,
                     const bool is_color)
{
  const int4 rect = tile.rect;
  const int w = tile.stride;
  const int h = rect.w - rect.y;
  const int channel_offset = is_color ? tile.pass_stride : 0;

  float *blur_difference = tile.temporary.get(0);
  float *difference = tile.temporary.get(tile.pass_stride);
  float *weight_accum = tile.temporary.get(2 * size_t(tile.pass_stride));

  memset(weight_accum, 0, sizeof(float) * w * h);
  memset(out, 0, sizeof(float) * w * h);

  for (int i = 0; i < (2 * r + 1) * (2 * r + 1); i++) {
    const int dy = i / (2 * r + 1) - r;
    const int dx = i % (2 * r + 1) - r;

    const int4 local_rect = make_int4(
        max(0, -dx), max(0, -dy), rect.z - rect.x - max(0, dx), rect.w - rect.y - max(0, dy));
    kernel_filter_nlm_calc_difference(
        dx, dy, guide, variance, nullptr, difference, local_rect, w, channel_offset, 0, a, k_2);
    kernel_filter_nlm_blur(difference, blur_difference, local_rect, w, f);
    kernel_filter_nlm_calc_weight(blur_difference, difference, local_rect, w, f);
    kernel_filter_nlm_blur(difference, blur_difference, local_rect, w, f);
    kernel_filter_nlm_update_output(dx,
                                    dy,
                                    blur_difference,
                                    image,
                                    difference,
                                    out,
                                    weight_accum,
                                    local_rect,
                                    channel_offset,
                                    w,
                                    f);
  }

  kernel_filter_nlm_normalize(
      out, weight_accum, make_int4(0, 0, rect.z - rect.x, rect.w - rect.y), w);
}

/* Load the guiding passes and smooth them (`DenoisingTask::prefilter_features`). The shadowing
 * feature of the original isn't available: constant, it doesn't change the result. */
void prefilter_features(NLMTile &tile, const NLMInput &input)
{
  const int4 rect = tile.rect;
  float *unfiltered = tile.pass(NLM_BUFFER_COLOR);
  float *variance = tile.pass(NLM_BUFFER_COLOR + 1);

  /* Offsets of the sums and of the sums of squares per feature, and where it goes. */
  struct Feature {
    int sum, squared, to;
  };
  const Feature features[7] = {
      {input.normal + 0, input.data + DENOISING_NLM_NORMAL_SQUARED + 0, NLM_BUFFER_NORMAL + 0},
      {input.normal + 1, input.data + DENOISING_NLM_NORMAL_SQUARED + 1, NLM_BUFFER_NORMAL + 1},
      {input.normal + 2, input.data + DENOISING_NLM_NORMAL_SQUARED + 2, NLM_BUFFER_NORMAL + 2},
      {input.data + DENOISING_NLM_DEPTH, input.data + DENOISING_NLM_DEPTH_SQUARED, NLM_BUFFER_DEPTH},
      {input.albedo + 0, input.data + DENOISING_NLM_ALBEDO_SQUARED + 0, NLM_BUFFER_ALBEDO + 0},
      {input.albedo + 1, input.data + DENOISING_NLM_ALBEDO_SQUARED + 1, NLM_BUFFER_ALBEDO + 1},
      {input.albedo + 2, input.data + DENOISING_NLM_ALBEDO_SQUARED + 2, NLM_BUFFER_ALBEDO + 2},
  };

  for (const Feature &feature : features) {
    for (int y = rect.y; y < rect.w; y++) {
      for (int x = rect.x; x < rect.z; x++) {
        const float *pixel = input.pixel(x, y);
        const int idx = (y - rect.y) * tile.stride + (x - rect.x);
        mean_and_variance(pixel[feature.sum],
                          pixel[feature.squared],
                          input.samples(pixel),
                          &unfiltered[idx],
                          &variance[idx]);
      }
    }
    non_local_means(
        tile, unfiltered, unfiltered, variance, tile.pass(feature.to), 2, 2, 1.0f, 0.25f, false);
  }

  std::fill_n(tile.pass(NLM_BUFFER_SHADOWING), tile.pass_stride, 1.0f);
}

/* Load the color and its variance, and remove outliers (`DenoisingTask::prefilter_color`). */
void prefilter_color(NLMTile &tile, const NLMInput &input)
{
  const int4 rect = tile.rect;
  FilterMemory color;
  color.alloc(6 * size_t(tile.pass_stride));

  for (int y = rect.y; y < rect.w; y++) {
    for (int x = rect.x; x < rect.z; x++) {
      const float *pixel = input.pixel(x, y);
      const int n = input.samples(pixel);
      const int idx = (y - rect.y) * tile.stride + (x - rect.x);
      for (int c = 0; c < 3; c++) {
        mean_and_variance(pixel[input.combined + c],
                          pixel[input.data + DENOISING_NLM_COLOR_SQUARED + c],
                          n,
                          color.get(c * size_t(tile.pass_stride) + idx),
                          color.get((3 + c) * size_t(tile.pass_stride) + idx));
      }
    }
  }

  for (int y = rect.y; y < rect.w; y++) {
    for (int x = rect.x; x < rect.z; x++) {
      kernel_filter_detect_outliers(x,
                                    y,
                                    color.get(),
                                    tile.pass(NLM_BUFFER_COLOR_VARIANCE),
                                    tile.pass(NLM_BUFFER_DEPTH),
                                    tile.pass(NLM_BUFFER_COLOR),
                                    rect,
                                    tile.pass_stride);
    }
  }
}

/* Reconstruct the pixels of the filter area into the output pass (`DenoisingTask::reconstruct`
 * and the CPU device's accumulate and solve). */
void reconstruct(NLMTile &tile,
                 const NLMInput &input,
                 float *output_buffer,
                 const int output,
                 const float nlm_k_2,
                 const float pca_threshold)
{
  const int4 rect = tile.rect;
  const int4 area = tile.filter_area;
  const int num_pixels = area.z * area.w;

  TileInfo tile_info = {};
  tile_info.num_frames = 1;

  vector<float> transform(size_t(num_pixels) * TRANSFORM_SIZE);
  vector<int> rank(num_pixels);
  for (int y = 0; y < area.w; y++) {
    for (int x = 0; x < area.z; x++) {
      const int idx = y * area.z + x;
      kernel_filter_construct_transform(tile.buffer.get(),
                                        &tile_info,
                                        x + area.x,
                                        y + area.y,
                                        rect,
                                        tile.pass_stride,
                                        NLM_BUFFER_NUM_PASSES * tile.pass_stride,
                                        false,
                                        transform.data() + size_t(idx) * TRANSFORM_SIZE,
                                        rank.data() + idx,
                                        NLM_RADIUS,
                                        pca_threshold);
    }
  }

  vector<float> XtWX(size_t(num_pixels) * XTWX_SIZE, 0.0f);
  vector<float3> XtWY(size_t(num_pixels) * XTWY_SIZE, zero_float3());

  const int4 filter_window = rect_from_shape(area.x - rect.x, area.y - rect.y, area.z, area.w);
  const int source_w = rect.z - rect.x;
  const int source_h = rect.w - rect.y;
  float *difference = tile.temporary.get(0);
  float *blur_difference = tile.temporary.get(tile.pass_stride);

  const int r = NLM_RADIUS;
  for (int i = 0; i < (2 * r + 1) * (2 * r + 1); i++) {
    const int dy = i / (2 * r + 1) - r;
    const int dx = i % (2 * r + 1) - r;

    const int4 local_rect = make_int4(
        max(0, -dx), max(0, -dy), source_w - max(0, dx), source_h - max(0, dy));
    kernel_filter_nlm_calc_difference(dx,
                                      dy,
                                      tile.pass(NLM_BUFFER_COLOR),
                                      tile.pass(NLM_BUFFER_COLOR_VARIANCE),
                                      nullptr,
                                      difference,
                                      local_rect,
                                      tile.stride,
                                      tile.pass_stride,
                                      0,
                                      1.0f,
                                      nlm_k_2);
    kernel_filter_nlm_blur(difference, blur_difference, local_rect, tile.stride, 4);
    kernel_filter_nlm_calc_weight(blur_difference, difference, local_rect, tile.stride, 4);
    kernel_filter_nlm_blur(difference, blur_difference, local_rect, tile.stride, 4);
    kernel_filter_nlm_construct_gramian(dx,
                                        dy,
                                        0,
                                        blur_difference,
                                        tile.buffer.get(),
                                        transform.data(),
                                        rank.data(),
                                        XtWX.data(),
                                        XtWY.data(),
                                        local_rect,
                                        filter_window,
                                        tile.stride,
                                        4,
                                        tile.pass_stride,
                                        0,
                                        false);
  }

  /* Solve (`kernel_filter_finalize`), writing to the output pass. */
  for (int y = 0; y < area.w; y++) {
    for (int x = 0; x < area.z; x++) {
      const int idx = y * area.z + x;
      const float *pixel = input.pixel(x + area.x, y + area.y);
      float *out = output_buffer + (pixel - input.buffer) + output;
      const float *noisy = pixel + input.combined;

      float *l_XtWX = XtWX.data() + size_t(idx) * XTWX_SIZE;
      float3 *l_XtWY = XtWY.data() + size_t(idx) * XTWY_SIZE;

      if (l_XtWX[0] < 1e-3f) {
        /* There is not enough information to determine a denoised result.
         * As a fallback, keep the original value of the pixel. */
        out[0] = noisy[0];
        out[1] = noisy[1];
        out[2] = noisy[2];
      }
      else {
        /* The weighted average of pixel colors (essentially, the NLM-filtered image).
         * In case the solution of the linear model fails due to numerical issues or
         * returns nonsensical negative values, fall back to this value. */
        const float3 mean_color = l_XtWY[0] / l_XtWX[0];

        math_trimatrix_vec3_solve(l_XtWX, l_XtWY, rank[idx] + 1, 1);

        float3 final_color = l_XtWY[0];
        if (!isfinite_safe(final_color) ||
            (final_color.x < -0.01f || final_color.y < -0.01f || final_color.z < -0.01f))
        {
          final_color = mean_color;
        }

        /* Clamp to positive values, reverse the highlight compression, and scale to the samples
         * like the other passes. */
        final_color = color_highlight_uncompress(max(final_color, zero_float3())) *
                      float(input.samples(pixel));
        out[0] = final_color.x;
        out[1] = final_color.y;
        out[2] = final_color.z;
      }
      out[3] = noisy[3];
    }
  }
}

/* Without the passes the denoiser needs, the denoised pass is the noisy one. */
void copy_noisy(const BufferParams &buffer_params,
                float *buffer,
                const PassType pass_type,
                const int num_components)
{
  const int noisy = buffer_params.get_pass_offset(pass_type);
  const int denoised = buffer_params.get_pass_offset(pass_type, PassMode::DENOISED);
  if (noisy == PASS_UNUSED || denoised == PASS_UNUSED) {
    return;
  }
  for (int64_t y = 0; y < buffer_params.height; y++) {
    for (int64_t x = 0; x < buffer_params.width; x++) {
      float *pixel = buffer + (buffer_params.offset + buffer_params.full_x + x +
                               (buffer_params.full_y + y) * buffer_params.stride) *
                                  buffer_params.pass_stride;
      std::copy_n(pixel + noisy, num_components, pixel + denoised);
    }
  }
}

}  // namespace

NLMDenoiser::NLMDenoiser(Device *denoiser_device, const DenoiseParams &params)
    : Denoiser(denoiser_device, params)
{
}

bool NLMDenoiser::denoise_buffer(const BufferParams &buffer_params,
                                 const BufferParams & /*denoised_buffer_params*/,
                                 RenderBuffers *render_buffers,
                                 const int num_samples,
                                 const bool /*allow_inplace_modification*/,
                                 const float2 /*pixel_jitter*/)
{
  /* Make sure the host-side data is available for denoising. */
  render_buffers->copy_from_device();
  float *buffer = render_buffers->buffer.data();

  NLMInput input;
  input.buffer = buffer;
  input.offset = buffer_params.offset + buffer_params.full_x +
                 int64_t(buffer_params.full_y) * buffer_params.stride;
  input.stride = buffer_params.stride;
  input.pass_stride = buffer_params.pass_stride;
  input.num_samples = num_samples;
  input.combined = buffer_params.get_pass_offset(PASS_COMBINED);
  input.normal = buffer_params.get_pass_offset(PASS_DENOISING_NORMAL);
  input.albedo = buffer_params.get_pass_offset(PASS_DENOISING_ALBEDO);
  input.data = buffer_params.get_pass_offset(PASS_DENOISING_NLM);
  input.sample_count = buffer_params.get_pass_offset(PASS_SAMPLE_COUNT);
  const int output = buffer_params.get_pass_offset(PASS_COMBINED, PassMode::DENOISED);

  /* Shadow catcher passes aren't denoised. */
  copy_noisy(buffer_params, buffer, PASS_SHADOW_CATCHER, 3);
  copy_noisy(buffer_params, buffer, PASS_SHADOW_CATCHER_MATTE, 4);

  if (output == PASS_UNUSED) {
    render_buffers->copy_to_device();
    return true;
  }
  if (input.combined == PASS_UNUSED || input.normal == PASS_UNUSED ||
      input.albedo == PASS_UNUSED || input.data == PASS_UNUSED)
  {
    LOG_WARNING << "NLM denoiser is missing its passes, the image is not denoised.";
    copy_noisy(buffer_params, buffer, PASS_COMBINED, 4);
    render_buffers->copy_to_device();
    return true;
  }

  const float nlm_k_2 = powf(2.0f, mix(-5.0f, 3.0f, NLM_STRENGTH));
  const float pca_threshold = powf(10.0f, mix(-5.0f, 3.0f, NLM_FEATURE_STRENGTH));
  const int width = buffer_params.width;
  const int height = buffer_params.height;
  const double start_time = time_dt();

  for (int tile_y = 0; tile_y < height; tile_y += NLM_TILE_SIZE) {
    for (int tile_x = 0; tile_x < width; tile_x += NLM_TILE_SIZE) {
      if (is_cancelled()) {
        return false;
      }

      NLMTile tile;
      tile.filter_area = make_int4(tile_x,
                                   tile_y,
                                   min(NLM_TILE_SIZE, width - tile_x),
                                   min(NLM_TILE_SIZE, height - tile_y));
      tile.rect = rect_clip(rect_expand(rect_from_shape(tile.filter_area.x,
                                                        tile.filter_area.y,
                                                        tile.filter_area.z,
                                                        tile.filter_area.w),
                                        NLM_RADIUS),
                            make_int4(0, 0, width, height));
      tile.stride = align_up(tile.rect.z - tile.rect.x, 4);
      tile.pass_stride = align_up(tile.stride * (tile.rect.w - tile.rect.y), 4);
      tile.buffer.alloc(NLM_BUFFER_NUM_PASSES * size_t(tile.pass_stride));
      tile.temporary.alloc(3 * size_t(tile.pass_stride));

      prefilter_features(tile, input);
      prefilter_color(tile, input);
      reconstruct(tile, input, buffer, output, nlm_k_2, pca_threshold);
    }
  }

  LOG_INFO << "NLM denoised " << width << "x" << height << " in " << time_dt() - start_time
           << " seconds.";

  render_buffers->copy_to_device();
  return true;
}

CCL_NAMESPACE_END
