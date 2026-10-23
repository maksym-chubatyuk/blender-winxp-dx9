/* SPDX-FileCopyrightText: 2011-2017 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Prefiltering of the NLM denoiser's color pass: outliers and highlight compression.
 * Restored from Cycles before 3.0 (the NLM denoiser), for builds without OpenImageDenoise. */

#pragma once

#include "util/color.h"
#include "util/math.h"
#include "util/types.h"

CCL_NAMESPACE_BEGIN

#define GET_COLOR(image) \
  make_float3(image[idx], image[idx + pass_stride], image[idx + 2 * pass_stride])
#define SET_COLOR(image, color) \
  image[idx] = color.x; \
  image[idx + pass_stride] = color.y; \
  image[idx + 2 * pass_stride] = color.z

ccl_device void kernel_filter_detect_outliers(int x,
                                              int y,
                                              ccl_global float *in,
                                              ccl_global float *variance_out,
                                              ccl_global float *depth,
                                              ccl_global float *image_out,
                                              int4 rect,
                                              int pass_stride)
{
  int buffer_w = align_up(rect.z - rect.x, 4);

  ccl_global float *image_in = in;
  ccl_global float *variance_in = in + 3 * pass_stride;

  int n = 0;
  float values[25];
  float pixel_variance, max_variance = 0.0f;
  for (int y1 = max(y - 2, rect.y); y1 < min(y + 3, rect.w); y1++) {
    for (int x1 = max(x - 2, rect.x); x1 < min(x + 3, rect.z); x1++) {
      int idx = (y1 - rect.y) * buffer_w + (x1 - rect.x);
      float3 color = GET_COLOR(image_in);
      color = max(color, make_float3(0.0f, 0.0f, 0.0f));
      float L = average(color);

      /* Find the position of L. */
      int i;
      for (i = 0; i < n; i++) {
        if (values[i] > L)
          break;
      }
      /* Make space for L by shifting all following values to the right. */
      for (int j = n; j > i; j--) {
        values[j] = values[j - 1];
      }
      /* Insert L. */
      values[i] = L;
      n++;

      float3 pixel_var = GET_COLOR(variance_in);
      float var = average(pixel_var);
      if ((x1 == x) && (y1 == y)) {
        pixel_variance = (pixel_var.x < 0.0f || pixel_var.y < 0.0f || pixel_var.z < 0.0f) ? -1.0f :
                                                                                            var;
      }
      else {
        max_variance = max(max_variance, var);
      }
    }
  }

  max_variance += 1e-4f;

  int idx = (y - rect.y) * buffer_w + (x - rect.x);

  float3 color = GET_COLOR(image_in);
  float3 variance = GET_COLOR(variance_in);
  color = max(color, make_float3(0.0f, 0.0f, 0.0f));
  variance = max(variance, make_float3(0.0f, 0.0f, 0.0f));

  float L = average(color);

  float ref = 2.0f * values[(int)(n * 0.75f)];

  /* Slightly offset values to avoid false positives in (almost) black areas. */
  max_variance += 1e-5f;
  ref -= 1e-5f;

  if (L > ref) {
    /* The pixel appears to be an outlier.
     * However, it may just be a legitimate highlight. Therefore, it is checked how likely it is
     * that the pixel should actually be at the reference value: If the reference is within the
     * 3-sigma interval, the pixel is assumed to be a statistical outlier. Otherwise, it is very
     * unlikely that the pixel should be darker, which indicates a legitimate highlight.
     */

    if (pixel_variance < 0.0f || pixel_variance > 9.0f * max_variance) {
      depth[idx] = -depth[idx];
      color *= ref / L;
      variance = make_float3(max_variance, max_variance, max_variance);
    }
    else {
      float stddev = sqrtf(pixel_variance);
      if (L - 3 * stddev < ref) {
        /* The pixel is an outlier, so negate the depth value to mark it as one.
         * Also, scale its brightness down to the outlier threshold to avoid trouble with the NLM
         * weights. */
        depth[idx] = -depth[idx];
        float fac = ref / L;
        color *= fac;
        variance *= sqr(fac);
      }
    }
  }

  /* Apply log(1+x) transform to compress highlights and avoid halos in the denoised results.
   * Variance is transformed accordingly - the derivative of the transform is 1/(1+x), so we
   * scale by the square of that (since we have variance instead of standard deviation). */
  color = color_highlight_compress(color, &variance);

  SET_COLOR(image_out, color);
  SET_COLOR(variance_out, variance);
}

#undef GET_COLOR
#undef SET_COLOR

CCL_NAMESPACE_END
