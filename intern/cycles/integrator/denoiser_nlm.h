/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "integrator/denoiser.h"

CCL_NAMESPACE_BEGIN

/* Cycles' non-local means denoiser from before Cycles 3.0, on the CPU: for builds without
 * OpenImageDenoise, which needs SSE4.1 and a 64-bit CPU (the Windows XP port, Pentium 4).
 *
 * The image is prefiltered (guiding passes and color, with outlier removal), then every pixel is
 * reconstructed by a weighted linear regression over its neighborhood in a feature space reduced
 * per pixel (PCA of depth, normal and albedo), the weights coming from non-local means on the
 * color. Works on tiles to bound memory. The shadowing feature of the original isn't available. */
class NLMDenoiser : public Denoiser {
 public:
  NLMDenoiser(Device *denoiser_device, const DenoiseParams &params);

  bool denoise_buffer(const BufferParams &buffer_params,
                      const BufferParams &denoised_buffer_params,
                      RenderBuffers *render_buffers,
                      int num_samples,
                      bool allow_inplace_modification,
                      float2 pixel_jitter) override;
};

CCL_NAMESPACE_END
