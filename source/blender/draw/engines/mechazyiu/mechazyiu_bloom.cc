/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * MECHAZYIU bloom on the CPU, for final renders (their tiles are only together on the CPU). The
 * viewport does the same on the GPU (`mechazyiu_bloom.hlsl`). The filters of EEVEE Legacy: the
 * bright parts at half the resolution, halved again and again (4 bilinear samples), added back up
 * level by level (tent filter of 9 bilinear samples).
 */

#include "BLI_array.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "mechazyiu_private.hh"

namespace blender::mechazyiu {

namespace {

struct Image {
  int2 size = int2(0);
  Array<float3> pixels;

  Image() = default;
  Image(int2 size) : size(size), pixels(size_t(size.x) * size.y, float3(0.0f)) {}

  float3 &at(int x, int y)
  {
    return pixels[size_t(y) * size.x + x];
  }

  /** Bilinear, clamped to the edges; `uv` in [0, 1]. */
  float3 sample(float2 uv) const
  {
    const float x = math::clamp(uv.x * size.x - 0.5f, 0.0f, float(size.x - 1));
    const float y = math::clamp(uv.y * size.y - 0.5f, 0.0f, float(size.y - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = math::min(x0 + 1, size.x - 1), y1 = math::min(y0 + 1, size.y - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    auto p = [&](int px, int py) { return pixels[size_t(py) * size.x + px]; };
    return math::interpolate(
        math::interpolate(p(x0, y0), p(x1, y0), fx), math::interpolate(p(x0, y1), p(x1, y1), fx), fy);
  }

  float2 texel() const
  {
    return 1.0f / float2(size);
  }
};

/** Each pixel of an image of `size` from its texture coordinates. */
template<typename Fn> static Image image_fill(int2 size, Fn fn)
{
  Image image(size);
  threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (int x = 0; x < size.x; x++) {
        image.at(x, y) = fn((float2(x, y) + 0.5f) / float2(size));
      }
    }
  });
  return image;
}

/** 4 bilinear samples one texel away diagonally: the mean of 4 x 4 texels. */
static float3 downsample(const Image &source, float2 uv)
{
  const float2 t = source.texel();
  return (source.sample(uv + float2(-t.x, -t.y)) + source.sample(uv + float2(t.x, -t.y)) +
          source.sample(uv + float2(-t.x, t.y)) + source.sample(uv + float2(t.x, t.y))) *
         0.25f;
}

/** Tent filter of 9 bilinear samples, `scale` texels of `source` apart. */
static float3 upsample(const Image &source, float2 uv, float scale)
{
  const float2 d = source.texel() * scale;
  float3 s = source.sample(uv) * 4.0f;
  s += (source.sample(uv + float2(0.0f, -d.y)) + source.sample(uv + float2(-d.x, 0.0f)) +
        source.sample(uv + float2(d.x, 0.0f)) + source.sample(uv + float2(0.0f, d.y))) *
       2.0f;
  s += source.sample(uv + float2(-d.x, -d.y)) + source.sample(uv + float2(d.x, -d.y)) +
       source.sample(uv + float2(-d.x, d.y)) + source.sample(uv + float2(d.x, d.y));
  return s * (1.0f / 16.0f);
}

}  // namespace

float3 bloom_prefilter(float3 color, const BloomSettings &settings)
{
  const float4 curve = settings.curve();
  const float brightness = math::max(math::reduce_max(color), 1e-5f);
  /* Under the threshold: a quadratic curve (the knee). */
  float soft = math::clamp(brightness - curve.x, 0.0f, curve.y);
  soft = curve.z * soft * soft;
  color *= math::max(soft, brightness - curve.w) / brightness;
  if (settings.clamp > 0.0f) {
    const float clamped = math::max(math::reduce_max(color), 1e-5f);
    color *= 1.0f - math::max(0.0f, clamped - settings.clamp) / clamped;
  }
  return color;
}

BloomLevels bloom_levels_get(int2 size, float radius)
{
  /* EEVEE Legacy: the iterations from the radius and the size at half the resolution. */
  const float min_dim = float(math::max(math::min(size.x, size.y) / 2, 1));
  const float iterations = (radius - 8.0f) + std::log2(min_dim);
  BloomLevels levels;
  levels.len = math::clamp(int(iterations), 1, bloom_levels_max);
  levels.sample_scale = 0.5f + iterations - float(int(iterations));
  return levels;
}

void bloom_apply(MutableSpan<float4> pixels, int2 size, const BloomSettings &settings)
{
  if (size.x < 4 || size.y < 4) {
    return;
  }
  const BloomLevels levels = bloom_levels_get(size, settings.radius);

  Image source(size);
  for (const int64_t i : pixels.index_range()) {
    source.pixels[i] = float3(pixels[i]);
  }
  /* Bright parts at half the resolution. */
  const Image bright = image_fill(math::max(size / 2, int2(1)), [&](float2 uv) {
    return bloom_prefilter(downsample(source, uv), settings);
  });

  Vector<Image> down;
  const Image *last = &bright;
  for (int i = 0; i < levels.len; i++) {
    down.append(image_fill(math::max(last->size / 2, int2(1)),
                           [&](float2 uv) { return downsample(*last, uv); }));
    last = &down.last();
  }
  Image up;
  last = &down.last();
  for (int i = levels.len - 2; i >= 0; i--) {
    up = image_fill(down[i].size, [&](float2 uv) {
      return down[i].sample(uv) + upsample(*last, uv, levels.sample_scale);
    });
    down[i] = std::move(up);
    last = &down[i];
  }

  const float3 color = settings.color * settings.intensity;
  threading::parallel_for(IndexRange(size.y), 16, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (int x = 0; x < size.x; x++) {
        const float2 uv = (float2(x, y) + 0.5f) / float2(size);
        float4 &pixel = pixels[size_t(y) * size.x + x];
        const float3 bloom = upsample(*last, uv, levels.sample_scale) * color;
        pixel = float4(float3(pixel) + bloom, pixel.w);
      }
    }
  });
}

}  // namespace blender::mechazyiu
