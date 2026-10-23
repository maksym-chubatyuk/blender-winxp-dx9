/* SPDX-FileCopyrightText: 2011-2017 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Weighted linear regression of the NLM denoiser (one neighbor at a time).
 * Restored from Cycles before 3.0 (the NLM denoiser), for builds without OpenImageDenoise. */

#pragma once

#include "integrator/nlm/features.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline void kernel_filter_construct_gramian(int x,
                                                       int y,
                                                       int storage_stride,
                                                       int dx,
                                                       int dy,
                                                       int t,
                                                       int buffer_stride,
                                                       int pass_stride,
                                                       int frame_offset,
                                                       bool use_time,
                                                       const ccl_global float *ccl_restrict buffer,
                                                       const ccl_global float *ccl_restrict
                                                           transform,
                                                       ccl_global int *rank,
                                                       float weight,
                                                       ccl_global float *XtWX,
                                                       ccl_global float3 *XtWY,
                                                       int localIdx)
{
  (void)localIdx; /* Only used on GPUs. */
  if (weight < 1e-3f) {
    return;
  }

  int p_offset = y * buffer_stride + x;
  int q_offset = (y + dy) * buffer_stride + (x + dx) + frame_offset;

#ifdef __KERNEL_GPU__
  const int stride = storage_stride;
#else
  const int stride = 1;
  (void)storage_stride;
#endif

#ifdef __KERNEL_CUDA__
  ccl_local float shared_design_row[(DENOISE_FEATURES + 1) * CCL_MAX_LOCAL_SIZE];
  ccl_local_param float *design_row = shared_design_row + localIdx * (DENOISE_FEATURES + 1);
#else
  float design_row[DENOISE_FEATURES + 1];
#endif

  float3 q_color = filter_get_color(buffer + q_offset, pass_stride);

  /* If the pixel was flagged as an outlier during prefiltering, skip it. */
  if (ccl_get_feature(buffer + q_offset, 0) < 0.0f) {
    return;
  }

  filter_get_design_row_transform(make_int3(x, y, t),
                                  buffer + p_offset,
                                  make_int3(x + dx, y + dy, t),
                                  buffer + q_offset,
                                  pass_stride,
                                  *rank,
                                  design_row,
                                  transform,
                                  stride,
                                  use_time);

#ifdef __KERNEL_GPU__
  math_trimatrix_add_gramian_strided(XtWX, (*rank) + 1, design_row, weight, stride);
  math_vec3_add_strided(XtWY, (*rank) + 1, design_row, weight * q_color, stride);
#else
  math_trimatrix_add_gramian(XtWX, (*rank) + 1, design_row, weight);
  math_vec3_add(XtWY, (*rank) + 1, design_row, weight * q_color);
#endif
}

CCL_NAMESPACE_END
