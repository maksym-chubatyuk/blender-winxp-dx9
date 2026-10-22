/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_state_private.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

class D3D9FrameBuffer;
class D3D9Texture;

/** Number of pixel shader samplers in SM2.0. */
constexpr int D3D9_MAX_SAMPLERS = 16;

class D3D9StateManager : public StateManager {
 public:
  /** Frame-buffer bound for drawing, it decides the face winding and Y flip. */
  D3D9FrameBuffer *active_fb = nullptr;

  explicit D3D9StateManager(IDirect3DDevice9 *device);

  void apply_state() override;
  void force_state() override;

  void issue_barrier(GPUBarrier /*barrier_bits*/) override {}

  void texture_bind(Texture *tex, GPUSamplerState sampler, int unit) override;
  void texture_unbind(Texture *tex) override;
  void texture_unbind_all() override;

  void image_bind(Texture * /*tex*/, int /*unit*/) override {}
  void image_unbind(Texture * /*tex*/) override {}
  void image_unbind_all() override {}

  /** Re-apply the culling as the winding depends on the frame-buffer orientation. */
  void framebuffer_orientation_changed();

  /** Bound texture for a sampler unit, null when none. */
  D3D9Texture *texture_get(int unit) const
  {
    return textures_[unit];
  }

 private:
  void set_state(const GPUState &state);
  void set_mutable_state(const GPUStateMutable &state);

  void set_blend(GPUBlend value);
  void set_write_mask(GPUWriteMask value);
  void set_depth_test(GPUDepthTest value);
  void set_stencil(GPUStencilTest test, GPUStencilOp operation, const GPUStateMutable &state);
  void set_culling(GPUFaceCullTest test, bool invert_facing);

  void sampler_apply(int unit, GPUSamplerState sampler);
  void texture_bind_apply();

  IDirect3DDevice9 *device_;

  GPUState current_;
  GPUStateMutable current_mutable_;
  bool current_flipped_ = false;

  D3D9Texture *textures_[D3D9_MAX_SAMPLERS] = {};
  GPUSamplerState samplers_[D3D9_MAX_SAMPLERS];
  /** What was last sent to the device, to skip redundant calls. */
  IDirect3DBaseTexture9 *device_textures_[D3D9_MAX_SAMPLERS] = {};
  uint32_t device_samplers_[D3D9_MAX_SAMPLERS];
  uint32_t dirty_textures_ = 0;
};

}  // namespace blender::gpu
