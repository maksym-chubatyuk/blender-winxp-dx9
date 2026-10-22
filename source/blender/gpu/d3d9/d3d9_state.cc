/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BLI_math_base.h"
#include "BLI_math_bits.h"

#include "d3d9_backend.hh"
#include "d3d9_framebuffer.hh"
#include "d3d9_state.hh"
#include "d3d9_texture.hh"

namespace blender::gpu {

static DWORD float_as_dword(float f)
{
  DWORD d;
  memcpy(&d, &f, sizeof(d));
  return d;
}

D3D9StateManager::D3D9StateManager(IDirect3DDevice9 *device) : device_(device)
{
  /* States that never change. */
  device_->SetRenderState(D3DRS_LIGHTING, FALSE);
  device_->SetRenderState(D3DRS_FOGENABLE, FALSE);
  device_->SetRenderState(D3DRS_DITHERENABLE, FALSE);
  device_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  device_->SetRenderState(D3DRS_POINTSIZE_MIN, float_as_dword(1.0f));
  device_->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);

  for (int i = 0; i < D3D9_MAX_SAMPLERS; i++) {
    samplers_[i] = GPUSamplerState::default_sampler();
    device_samplers_[i] = UINT32_MAX;
  }
  this->force_state();
}

void D3D9StateManager::apply_state()
{
  if (active_fb && active_fb->is_flipped() != current_flipped_) {
    current_flipped_ = active_fb->is_flipped();
    set_culling(GPUFaceCullTest(state.culling_test), state.invert_facing);
  }
  this->set_state(this->state);
  this->set_mutable_state(this->mutable_state);
  this->texture_bind_apply();
}

void D3D9StateManager::force_state()
{
  current_ = ~this->state;
  current_mutable_ = ~this->mutable_state;
  for (int i = 0; i < D3D9_MAX_SAMPLERS; i++) {
    device_textures_[i] = nullptr;
    device_samplers_[i] = UINT32_MAX;
    device_->SetTexture(i, nullptr);
  }
  dirty_textures_ = ~0u;
  this->set_state(this->state);
  this->set_mutable_state(this->mutable_state);
}

void D3D9StateManager::framebuffer_orientation_changed()
{
  set_culling(GPUFaceCullTest(state.culling_test), state.invert_facing);
}

void D3D9StateManager::set_state(const GPUState &state)
{
  GPUState changed = state ^ current_;

  if (changed.blend != 0) {
    set_blend(GPUBlend(state.blend));
  }
  if (changed.write_mask != 0) {
    set_write_mask(GPUWriteMask(state.write_mask));
  }
  if (changed.depth_test != 0) {
    set_depth_test(GPUDepthTest(state.depth_test));
  }
  if (changed.stencil_test != 0 || changed.stencil_op != 0) {
    set_stencil(GPUStencilTest(state.stencil_test), GPUStencilOp(state.stencil_op), mutable_state);
  }
  if (changed.culling_test != 0 || changed.invert_facing != 0) {
    set_culling(GPUFaceCullTest(state.culling_test), state.invert_facing);
  }
  if (changed.line_smooth != 0) {
    device_->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, state.line_smooth ? TRUE : FALSE);
  }
  /* Not supported: logic operations, provoking vertex, clip distances (SM2.0 vertex shaders can't
   * output them), polygon smoothing. */

  current_ = state;
}

void D3D9StateManager::set_mutable_state(const GPUStateMutable &state)
{
  GPUStateMutable changed = state ^ current_mutable_;

  if (float_as_uint(changed.point_size) != 0) {
    /* Positive sizes are written by the shader (`PSIZE` output overrides this state). */
    device_->SetRenderState(D3DRS_POINTSIZE, float_as_dword(fabsf(state.point_size)));
  }
  /* Line width: D3D9 only rasterizes 1 pixel wide lines. */
  if (changed.stencil_compare_mask != 0 || changed.stencil_reference != 0 ||
      changed.stencil_write_mask != 0)
  {
    set_stencil(GPUStencilTest(current_.stencil_test), GPUStencilOp(current_.stencil_op), state);
  }

  current_mutable_ = state;
}

/* -------------------------------------------------------------------- */
/** \name Pipeline state
 * \{ */

void D3D9StateManager::set_blend(const GPUBlend value)
{
  DWORD src_rgb, dst_rgb, src_alpha, dst_alpha;
  switch (value) {
    default:
    case GPU_BLEND_ALPHA:
      src_rgb = D3DBLEND_SRCALPHA;
      dst_rgb = D3DBLEND_INVSRCALPHA;
      src_alpha = D3DBLEND_ONE;
      dst_alpha = D3DBLEND_INVSRCALPHA;
      break;
    case GPU_BLEND_ALPHA_PREMULT:
      src_rgb = D3DBLEND_ONE;
      dst_rgb = D3DBLEND_INVSRCALPHA;
      src_alpha = D3DBLEND_ONE;
      dst_alpha = D3DBLEND_INVSRCALPHA;
      break;
    case GPU_BLEND_ADDITIVE:
      src_rgb = D3DBLEND_SRCALPHA;
      dst_rgb = D3DBLEND_ONE;
      src_alpha = D3DBLEND_ZERO;
      dst_alpha = D3DBLEND_ONE;
      break;
    case GPU_BLEND_MIN:
    case GPU_BLEND_MAX:
    case GPU_BLEND_SUBTRACT:
    case GPU_BLEND_ADDITIVE_PREMULT:
      src_rgb = D3DBLEND_ONE;
      dst_rgb = D3DBLEND_ONE;
      src_alpha = D3DBLEND_ONE;
      dst_alpha = D3DBLEND_ONE;
      break;
    case GPU_BLEND_MULTIPLY:
      src_rgb = D3DBLEND_DESTCOLOR;
      dst_rgb = D3DBLEND_ZERO;
      src_alpha = D3DBLEND_DESTALPHA;
      dst_alpha = D3DBLEND_ZERO;
      break;
    case GPU_BLEND_INVERT:
      src_rgb = D3DBLEND_INVDESTCOLOR;
      dst_rgb = D3DBLEND_ZERO;
      src_alpha = D3DBLEND_ZERO;
      dst_alpha = D3DBLEND_ONE;
      break;
    case GPU_BLEND_OIT:
      src_rgb = D3DBLEND_ONE;
      dst_rgb = D3DBLEND_ONE;
      src_alpha = D3DBLEND_ZERO;
      dst_alpha = D3DBLEND_INVSRCALPHA;
      break;
    case GPU_BLEND_BACKGROUND:
      src_rgb = D3DBLEND_INVDESTALPHA;
      dst_rgb = D3DBLEND_SRCALPHA;
      src_alpha = D3DBLEND_ZERO;
      dst_alpha = D3DBLEND_SRCALPHA;
      break;
    case GPU_BLEND_ALPHA_UNDER_PREMUL:
      src_rgb = D3DBLEND_INVDESTALPHA;
      dst_rgb = D3DBLEND_ONE;
      src_alpha = D3DBLEND_INVDESTALPHA;
      dst_alpha = D3DBLEND_ONE;
      break;
    case GPU_BLEND_CUSTOM:
      /* Dual source blending is not available, approximate with pre-multiplied alpha. */
      src_rgb = D3DBLEND_ONE;
      dst_rgb = D3DBLEND_INVSRCALPHA;
      src_alpha = D3DBLEND_ONE;
      dst_alpha = D3DBLEND_INVSRCALPHA;
      break;
    case GPU_BLEND_OVERLAY_MASK_FROM_ALPHA:
      src_rgb = D3DBLEND_ZERO;
      dst_rgb = D3DBLEND_INVSRCALPHA;
      src_alpha = D3DBLEND_ZERO;
      dst_alpha = D3DBLEND_INVSRCALPHA;
      break;
    case GPU_BLEND_TRANSPARENCY:
      src_rgb = D3DBLEND_ONE;
      dst_rgb = D3DBLEND_SRCALPHA;
      src_alpha = D3DBLEND_ZERO;
      dst_alpha = D3DBLEND_SRCALPHA;
      break;
  }

  D3D9Backend *backend = D3D9Backend::get();
  if (backend->supports_blend_op) {
    DWORD op = D3DBLENDOP_ADD;
    if (value == GPU_BLEND_MIN) {
      op = D3DBLENDOP_MIN;
    }
    else if (value == GPU_BLEND_MAX) {
      op = D3DBLENDOP_MAX;
    }
    else if (value == GPU_BLEND_SUBTRACT) {
      op = D3DBLENDOP_REVSUBTRACT;
    }
    device_->SetRenderState(D3DRS_BLENDOP, op);
  }
  device_->SetRenderState(D3DRS_SRCBLEND, src_rgb);
  device_->SetRenderState(D3DRS_DESTBLEND, dst_rgb);
  if (backend->supports_separate_alpha_blend) {
    device_->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    device_->SetRenderState(D3DRS_SRCBLENDALPHA, src_alpha);
    device_->SetRenderState(D3DRS_DESTBLENDALPHA, dst_alpha);
    if (backend->supports_blend_op) {
      device_->SetRenderState(D3DRS_BLENDOPALPHA, value == GPU_BLEND_SUBTRACT ?
                                                      D3DBLENDOP_REVSUBTRACT :
                                                      D3DBLENDOP_ADD);
    }
  }
  device_->SetRenderState(D3DRS_ALPHABLENDENABLE, value != GPU_BLEND_NONE ? TRUE : FALSE);
}

void D3D9StateManager::set_write_mask(const GPUWriteMask value)
{
  device_->SetRenderState(D3DRS_ZWRITEENABLE, (value & GPU_WRITE_DEPTH) ? TRUE : FALSE);
  DWORD color = 0;
  if (value & GPU_WRITE_RED) {
    color |= D3DCOLORWRITEENABLE_RED;
  }
  if (value & GPU_WRITE_GREEN) {
    color |= D3DCOLORWRITEENABLE_GREEN;
  }
  if (value & GPU_WRITE_BLUE) {
    color |= D3DCOLORWRITEENABLE_BLUE;
  }
  if (value & GPU_WRITE_ALPHA) {
    color |= D3DCOLORWRITEENABLE_ALPHA;
  }
  device_->SetRenderState(D3DRS_COLORWRITEENABLE, color);
}

void D3D9StateManager::set_depth_test(const GPUDepthTest value)
{
  DWORD func;
  switch (value) {
    case GPU_DEPTH_LESS:
      func = D3DCMP_LESS;
      break;
    case GPU_DEPTH_LESS_EQUAL:
      func = D3DCMP_LESSEQUAL;
      break;
    case GPU_DEPTH_EQUAL:
      func = D3DCMP_EQUAL;
      break;
    case GPU_DEPTH_GREATER:
      func = D3DCMP_GREATER;
      break;
    case GPU_DEPTH_GREATER_EQUAL:
      func = D3DCMP_GREATEREQUAL;
      break;
    case GPU_DEPTH_ALWAYS:
    default:
      func = D3DCMP_ALWAYS;
      break;
  }
  device_->SetRenderState(D3DRS_ZENABLE, value != GPU_DEPTH_NONE ? D3DZB_TRUE : D3DZB_FALSE);
  device_->SetRenderState(D3DRS_ZFUNC, func);
}

void D3D9StateManager::set_stencil(const GPUStencilTest test,
                                   const GPUStencilOp operation,
                                   const GPUStateMutable &state)
{
  if (test == GPU_STENCIL_NONE) {
    device_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device_->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    return;
  }

  DWORD func = D3DCMP_ALWAYS;
  switch (test) {
    case GPU_STENCIL_NEQUAL:
      func = D3DCMP_NOTEQUAL;
      break;
    case GPU_STENCIL_EQUAL:
      func = D3DCMP_EQUAL;
      break;
    default:
      break;
  }
  device_->SetRenderState(D3DRS_STENCILENABLE, TRUE);
  device_->SetRenderState(D3DRS_STENCILFUNC, func);
  device_->SetRenderState(D3DRS_CCW_STENCILFUNC, func);
  device_->SetRenderState(D3DRS_STENCILREF, state.stencil_reference);
  device_->SetRenderState(D3DRS_STENCILMASK, state.stencil_compare_mask);
  device_->SetRenderState(D3DRS_STENCILWRITEMASK, state.stencil_write_mask);

  DWORD fail = D3DSTENCILOP_KEEP, zfail = D3DSTENCILOP_KEEP, pass = D3DSTENCILOP_KEEP;
  DWORD ccw_zfail = D3DSTENCILOP_KEEP, ccw_pass = D3DSTENCILOP_KEEP;
  bool two_sided = false;
  switch (operation) {
    case GPU_STENCIL_OP_REPLACE:
      pass = D3DSTENCILOP_REPLACE;
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_PASS:
      /* Back faces increment, front faces decrement. */
      two_sided = true;
      pass = D3DSTENCILOP_DECR;
      ccw_pass = D3DSTENCILOP_INCR;
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_FAIL:
      two_sided = true;
      zfail = D3DSTENCILOP_INCR;
      ccw_zfail = D3DSTENCILOP_DECR;
      break;
    case GPU_STENCIL_OP_NONE:
    default:
      break;
  }
  device_->SetRenderState(D3DRS_STENCILFAIL, fail);
  device_->SetRenderState(D3DRS_STENCILZFAIL, zfail);
  device_->SetRenderState(D3DRS_STENCILPASS, pass);
  device_->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, two_sided ? TRUE : FALSE);
  if (two_sided) {
    device_->SetRenderState(D3DRS_CCW_STENCILFAIL, fail);
    device_->SetRenderState(D3DRS_CCW_STENCILZFAIL, ccw_zfail);
    device_->SetRenderState(D3DRS_CCW_STENCILPASS, ccw_pass);
  }
}

void D3D9StateManager::set_culling(const GPUFaceCullTest test, const bool invert_facing)
{
  if (test == GPU_CULL_NONE) {
    device_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    return;
  }
  /* OpenGL front faces are counter-clockwise in a Y up window, so they are clockwise in the Y down
   * D3D window. Flipped render targets swap the winding again. */
  const bool front_is_cw = (!invert_facing) != current_flipped_;
  const bool cull_cw = (test == GPU_CULL_FRONT) ? front_is_cw : !front_is_cw;
  device_->SetRenderState(D3DRS_CULLMODE, cull_cw ? D3DCULL_CW : D3DCULL_CCW);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture binding
 * \{ */

void D3D9StateManager::texture_bind(Texture *tex, GPUSamplerState sampler, int unit)
{
  if (unit < 0 || unit >= D3D9_MAX_SAMPLERS) {
    return;
  }
  textures_[unit] = static_cast<D3D9Texture *>(tex);
  samplers_[unit] = sampler;
  dirty_textures_ |= 1u << unit;
}

void D3D9StateManager::texture_unbind(Texture *tex)
{
  for (int i = 0; i < D3D9_MAX_SAMPLERS; i++) {
    if (textures_[i] == tex) {
      textures_[i] = nullptr;
      dirty_textures_ |= 1u << i;
    }
  }
}

void D3D9StateManager::texture_unbind_all()
{
  for (int i = 0; i < D3D9_MAX_SAMPLERS; i++) {
    if (textures_[i]) {
      textures_[i] = nullptr;
      dirty_textures_ |= 1u << i;
    }
  }
}

static DWORD to_d3d9_address(GPUSamplerExtendMode mode)
{
  switch (mode) {
    case GPU_SAMPLER_EXTEND_MODE_REPEAT:
      return D3DTADDRESS_WRAP;
    case GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT:
      return D3DTADDRESS_MIRROR;
    case GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER:
      return D3DTADDRESS_BORDER;
    case GPU_SAMPLER_EXTEND_MODE_EXTEND:
    default:
      return D3DTADDRESS_CLAMP;
  }
}

void D3D9StateManager::sampler_apply(int unit, GPUSamplerState sampler)
{
  D3D9Texture *tex = textures_[unit];
  const bool has_mips = tex && tex->device_levels() > 1;
  const bool can_filter = tex == nullptr || tex->is_filterable();
  const bool is_pow2 = tex == nullptr || (is_power_of_2_i(tex->device_width()) &&
                                          is_power_of_2_i(tex->device_height()));
  /* Render targets store linear values (there is no sRGB encoding on write), only uploaded sRGB
   * data is decoded. */
  const bool srgb = tex && tex->format_get() == TextureFormat::SRGBA_8_8_8_8 &&
                    !tex->is_render_target();

  /* Pack everything into a key to skip redundant state changes. */
  uint32_t key = uint32_t(sampler.filtering) | uint32_t(sampler.extend_x) << 8 |
                 uint32_t(sampler.extend_yz) << 12 | uint32_t(sampler.custom_type) << 16 |
                 uint32_t(sampler.type) << 20 | uint32_t(has_mips) << 24 |
                 uint32_t(can_filter) << 25 | uint32_t(is_pow2) << 26 | uint32_t(srgb) << 27;
  if (device_samplers_[unit] == key) {
    return;
  }
  device_samplers_[unit] = key;

  DWORD mag = D3DTEXF_POINT, min = D3DTEXF_POINT, mip = D3DTEXF_NONE;
  DWORD max_aniso = 1;
  float lod_bias = 0.0f;
  DWORD address_u, address_v;

  if (sampler.type == GPU_SAMPLER_STATE_TYPE_CUSTOM &&
      sampler.custom_type == GPU_SAMPLER_CUSTOM_ICON)
  {
    mag = min = D3DTEXF_LINEAR;
    mip = D3DTEXF_POINT;
    lod_bias = -0.5f;
    address_u = address_v = D3DTADDRESS_CLAMP;
  }
  else {
    const bool linear = (sampler.filtering & GPU_SAMPLER_FILTERING_LINEAR) != 0;
    mag = min = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    if (sampler.filtering & GPU_SAMPLER_FILTERING_MIPMAP) {
      mip = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    }
    if ((sampler.filtering & GPU_SAMPLER_FILTERING_ANISOTROPIC_MASK) &&
        D3D9Backend::get()->caps.MaxAnisotropy > 1)
    {
      min = D3DTEXF_ANISOTROPIC;
      const int level = (sampler.filtering & GPU_SAMPLER_FILTERING_ANISOTROPIC_MASK) >> 2;
      max_aniso = min_uu(1u << level, D3D9Backend::get()->caps.MaxAnisotropy);
    }
    address_u = to_d3d9_address(sampler.extend_x);
    address_v = to_d3d9_address(sampler.extend_yz);
  }
  if (!has_mips) {
    mip = D3DTEXF_NONE;
  }
  if (!can_filter) {
    mag = min = D3DTEXF_POINT;
    mip = (mip == D3DTEXF_NONE) ? D3DTEXF_NONE : D3DTEXF_POINT;
  }
  if (!is_pow2) {
    /* Conditional non power of two support: clamp addressing only. */
    address_u = address_v = D3DTADDRESS_CLAMP;
  }

  device_->SetSamplerState(unit, D3DSAMP_MAGFILTER, mag);
  device_->SetSamplerState(unit, D3DSAMP_MINFILTER, min);
  device_->SetSamplerState(unit, D3DSAMP_MIPFILTER, mip);
  device_->SetSamplerState(unit, D3DSAMP_MAXANISOTROPY, max_aniso);
  device_->SetSamplerState(unit, D3DSAMP_MIPMAPLODBIAS, float_as_dword(lod_bias));
  device_->SetSamplerState(unit, D3DSAMP_ADDRESSU, address_u);
  device_->SetSamplerState(unit, D3DSAMP_ADDRESSV, address_v);
  device_->SetSamplerState(unit, D3DSAMP_ADDRESSW, address_v);
  device_->SetSamplerState(unit, D3DSAMP_BORDERCOLOR, 0);
  device_->SetSamplerState(unit, D3DSAMP_SRGBTEXTURE, srgb ? TRUE : FALSE);
}

bool D3D9StateManager::is_attached(IDirect3DBaseTexture9 *base) const
{
  if (active_fb == nullptr) {
    return false;
  }
  for (int slot = 0; slot < D3D9_MAX_RENDER_TARGETS; slot++) {
    Texture *tex = active_fb->color_tex(slot);
    if (tex && static_cast<D3D9Texture *>(tex)->base_texture_get() == base) {
      return true;
    }
  }
  return false;
}

void D3D9StateManager::texture_bind_apply()
{
  for (int unit = 0; unit < D3D9_MAX_SAMPLERS; unit++) {
    /* Bound textures are checked every time: their device texture is created lazily and can
     * change when they become render targets. */
    if ((dirty_textures_ & (1u << unit)) == 0 && textures_[unit] == nullptr) {
      continue;
    }
    D3D9Texture *tex = textures_[unit];
    IDirect3DBaseTexture9 *base = tex ? tex->base_texture_get() : nullptr;
    if (base && is_attached(base)) {
      /* Sampling a render target of the draw is undefined: drivers drop the draw's writes. */
      base = nullptr;
    }
    if (device_textures_[unit] != base) {
      device_->SetTexture(unit, base);
      device_textures_[unit] = base;
    }
    if (tex) {
      sampler_apply(unit, samplers_[unit]);
    }
  }
  dirty_textures_ = 0;
}

/** \} */

}  // namespace blender::gpu
