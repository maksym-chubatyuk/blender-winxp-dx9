/* SPDX-FileCopyrightText: 2011-2017 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Sizes of the NLM denoiser's feature space.
 * Restored from Cycles before 3.0 (the NLM denoiser), for builds without OpenImageDenoise. */

#pragma once

#include "util/types.h"

CCL_NAMESPACE_BEGIN

#define DENOISE_FEATURES 11
#define TRANSFORM_SIZE (DENOISE_FEATURES * DENOISE_FEATURES)
#define XTWX_SIZE (((DENOISE_FEATURES + 1) * (DENOISE_FEATURES + 2)) / 2)
#define XTWY_SIZE (DENOISE_FEATURES + 1)

#define DENOISE_MAX_FRAMES 16

/* Frames of the denoised neighborhood (only the current frame is used). */
struct TileInfo {
  int frames[DENOISE_MAX_FRAMES];
  int num_frames;
};

#define CCL_FILTER_TILE_INFO TileInfo *tile_info

CCL_NAMESPACE_END
