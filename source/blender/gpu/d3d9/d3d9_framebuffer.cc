/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstdio>
#include <cstring>

#include "BLI_math_base.h"
#include "BLI_math_half.hh"

#include "GPU_framebuffer.hh"

#include "gpu_shader_private.hh"

#include "d3d9_backend.hh"
#include "d3d9_context.hh"
#include "d3d9_framebuffer.hh"
#include "d3d9_state.hh"
#include "d3d9_texture.hh"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.d3d9"};

/* -------------------------------------------------------------------- */
/** \name Creation
 * \{ */

D3D9FrameBuffer::D3D9FrameBuffer(const char *name) : FrameBuffer(name) {}

D3D9FrameBuffer::D3D9FrameBuffer(const char *name, D3D9Context *context)
    : FrameBuffer(name), context_(context), is_window_(true)
{
  set_color_attachment_bit(GPU_FB_COLOR_ATTACHMENT0, true);
  dirty_attachments_ = false;
}

D3D9FrameBuffer::~D3D9FrameBuffer()
{
  D3D9Context *ctx = D3D9Context::get();
  if (ctx && ctx->active_fb == this && ctx->back_left != this) {
    GPU_framebuffer_restore();
  }
  if (ctx && ctx->state_manager_get().active_fb == this) {
    ctx->state_manager_get().active_fb = nullptr;
  }
}

void D3D9FrameBuffer::window_surface_set(IDirect3DSurface9 *surface, int width, int height)
{
  if (surface == window_surface_ && width == width_ && height == height_) {
    return;
  }
  const bool first_time = width_ == 0 && height_ == 0;
  window_surface_ = surface;
  size_set(width, height);
  if (first_time) {
    viewport_reset();
    scissor_reset();
  }
  dirty_targets_ = true;
}

bool D3D9FrameBuffer::check(char err_out[256])
{
  bool has_color = false;
  for (int i = 0; i < GPU_FB_MAX_COLOR_ATTACHMENT; i++) {
    if (attachments_[GPU_FB_COLOR_ATTACHMENT0 + i].tex) {
      has_color = true;
      if (i >= D3D9_MAX_RENDER_TARGETS) {
        const char *format = "gpu::FrameBuffer: %s uses more than 4 color attachments\n";
        if (err_out) {
          snprintf(err_out, 256, format, name_);
        }
        else {
          fprintf(stderr, format, name_);
        }
        return false;
      }
    }
  }
  if (!has_color && depth_tex() == nullptr && !is_window_) {
    /* Empty frame-buffers are valid with a default size. */
    return width_ > 0 && height_ > 0;
  }
  return true;
}

void D3D9FrameBuffer::update_attachments()
{
  GPUAttachmentType first_attachment = GPU_FB_MAX_ATTACHMENT;
  for (GPUAttachmentType type = GPU_FB_MAX_ATTACHMENT - 1; type >= 0; --type) {
    if (attachments_[type].tex == nullptr) {
      continue;
    }
    if (type >= GPU_FB_COLOR_ATTACHMENT0 || first_attachment == GPU_FB_MAX_ATTACHMENT) {
      first_attachment = type;
    }
  }
  if (first_attachment != GPU_FB_MAX_ATTACHMENT) {
    GPUAttachment &attach = attachments_[first_attachment];
    int size[3];
    GPU_texture_get_mipmap_size(attach.tex, max_ii(attach.mip, 0), size);
    this->size_set(size[0], size[1]);
    srgb_ = GPU_texture_format(attach.tex) == TextureFormat::SRGBA_8_8_8_8;
  }
  dirty_attachments_ = false;
  dirty_targets_ = true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Binding
 * \{ */

void D3D9FrameBuffer::bind(bool enabled_srgb)
{
  D3D9Context *ctx = D3D9Context::get();
  if (context_ == nullptr) {
    context_ = ctx;
  }
  if (dirty_attachments_) {
    update_attachments();
    viewport_reset();
    scissor_reset();
  }
  if (ctx->active_fb != this) {
    ctx->active_fb = this;
    ctx->state_manager_get().active_fb = this;
    dirty_targets_ = true;
    dirty_state_ = true;
  }
  if (enabled_srgb_ != enabled_srgb) {
    enabled_srgb_ = enabled_srgb;
  }
  /* sRGB encoding is done by the shaders (`srgbTarget`). */
  Shader::set_framebuffer_srgb_target(enabled_srgb && srgb_);
}

IDirect3DSurface9 *D3D9FrameBuffer::color_surface(int slot)
{
  const GPUAttachment &attach = attachments_[GPU_FB_COLOR_ATTACHMENT0 + slot];
  if (attach.tex == nullptr) {
    return nullptr;
  }
  return static_cast<D3D9Texture *>(attach.tex)->surface_get(max_ii(attach.mip, 0), attach.layer);
}

IDirect3DSurface9 *D3D9FrameBuffer::depth_surface()
{
  const GPUAttachment &attach = depth_attachment();
  if (attach.tex == nullptr) {
    return nullptr;
  }
  return static_cast<D3D9Texture *>(attach.tex)->surface_get(0, 0);
}

void D3D9FrameBuffer::to_device_rect(const int rect[4], RECT &r_rect) const
{
  int x = rect[0], y = rect[1], w = rect[2], h = rect[3];
  if (!is_flipped()) {
    y = height_ - (y + h);
  }
  r_rect.left = clamp_i(x, 0, width_);
  r_rect.top = clamp_i(y, 0, height_);
  r_rect.right = clamp_i(x + w, 0, width_);
  r_rect.bottom = clamp_i(y + h, 0, height_);
}

void D3D9FrameBuffer::apply_state(const bool blending)
{
  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  D3D9Backend *backend = D3D9Backend::get();

  const bool single_target = blending && !backend->supports_mrt_blending;
  if (single_target != single_target_) {
    single_target_ = single_target;
    dirty_targets_ = true;
  }

  if (dirty_attachments_) {
    update_attachments();
  }

  if (dirty_targets_) {
    const int max_targets = clamp_i(int(backend->caps.NumSimultaneousRTs), 1, 4);
    if (is_window_) {
      IDirect3DSurface9 *target = window_surface_ ? window_surface_ :
                                                    backend->dummy_color_target(1, 1);
      device->SetRenderTarget(0, target);
      for (int i = 1; i < max_targets; i++) {
        device->SetRenderTarget(i, nullptr);
      }
      device->SetDepthStencilSurface(nullptr);
    }
    else {
      IDirect3DSurface9 *depth = depth_surface();
      for (int i = 0; i < max_targets; i++) {
        IDirect3DSurface9 *surface = (i > 0 && single_target_) ? nullptr : color_surface(i);
        if (i == 0 && surface == nullptr) {
          /* A render target is always needed. */
          surface = backend->dummy_color_target(max_ii(width_, 1), max_ii(height_, 1));
          if (surface) {
            surface->AddRef();
          }
        }
        device->SetRenderTarget(i, surface);
        d3d9_release(surface);
      }
      device->SetDepthStencilSurface(depth);
      d3d9_release(depth);
    }
    dirty_targets_ = false;
    /* Setting a render target resets the viewport. */
    dirty_state_ = true;
  }

  if (dirty_state_) {
    RECT rect;
    to_device_rect(viewport_[0], rect);
    D3DVIEWPORT9 vp;
    vp.X = rect.left;
    vp.Y = rect.top;
    vp.Width = max_ii(rect.right - rect.left, 1);
    vp.Height = max_ii(rect.bottom - rect.top, 1);
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    device->SetViewport(&vp);

    if (backend->supports_scissor) {
      to_device_rect(scissor_, rect);
      device->SetScissorRect(&rect);
      device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor_test_ ? TRUE : FALSE);
    }
    dirty_state_ = false;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clear
 * \{ */

static D3DCOLOR to_d3d9_color(const double4 &color)
{
  auto to_byte = [](double v) { return DWORD(clamp_f(float(v), 0.0f, 1.0f) * 255.0f + 0.5f); };
  return D3DCOLOR_ARGB(to_byte(color[3]), to_byte(color[0]), to_byte(color[1]), to_byte(color[2]));
}

/** D3D9 clears the viewport while OpenGL clears the whole target (minus the scissor). */
static void clear_full_viewport(IDirect3DDevice9 *device,
                                DWORD flags,
                                D3DCOLOR color,
                                float depth,
                                DWORD stencil)
{
  D3DVIEWPORT9 old_vp;
  device->GetViewport(&old_vp);
  IDirect3DSurface9 *target = nullptr;
  device->GetRenderTarget(0, &target);
  if (target) {
    D3DSURFACE_DESC desc;
    target->GetDesc(&desc);
    D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    device->SetViewport(&vp);
    target->Release();
  }
  const double gpu_start_ms = d3d9_gpu_time_enabled() ? d3d9_gpu_time_begin(device) : 0.0;
  const double start_ms = d3d9_time_ms();
  D3D9_CHECK(device->Clear(0, nullptr, flags, color, depth, stencil));
  d3d9_frame_stats.clear_ms += d3d9_time_ms() - start_ms;
  if (d3d9_gpu_time_enabled()) {
    char name[64] = "(clear)";
    IDirect3DSurface9 *rt = nullptr;
    device->GetRenderTarget(0, &rt);
    if (rt) {
      D3DSURFACE_DESC desc;
      rt->GetDesc(&desc);
      snprintf(name,
               sizeof(name),
               "(clear %ux%u fmt %d%s%s)",
               desc.Width,
               desc.Height,
               int(desc.Format),
               (flags & D3DCLEAR_TARGET) ? " color" : "",
               (flags & D3DCLEAR_ZBUFFER) ? " depth" : "");
      rt->Release();
    }
    d3d9_gpu_time_end(device, name, gpu_start_ms);
  }
  device->SetViewport(&old_vp);
}

void D3D9FrameBuffer::clear(GPUFrameBufferBits buffers,
                            const double4 clear_color,
                            float clear_depth,
                            uint clear_stencil)
{
  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  this->apply_state();

  DWORD flags = 0;
  if (buffers & GPU_COLOR_BIT) {
    flags |= D3DCLEAR_TARGET;
  }
  const GPUAttachment &depth = depth_attachment();
  if (depth.tex) {
    if (buffers & GPU_DEPTH_BIT) {
      flags |= D3DCLEAR_ZBUFFER;
    }
    if ((buffers & GPU_STENCIL_BIT) &&
        GPU_texture_format(depth.tex) == TextureFormat::SFLOAT_32_DEPTH_UINT_8)
    {
      flags |= D3DCLEAR_STENCIL;
    }
  }
  if (flags == 0) {
    return;
  }
  /* Color write mask doesn't apply to clears in OpenGL either when Blender clears. */
  clear_full_viewport(device, flags, to_d3d9_color(clear_color), clear_depth, clear_stencil);
}

void D3D9FrameBuffer::clear_attachment(GPUAttachmentType type, const double4 clear_value)
{
  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  this->apply_state();

  if (ELEM(type, GPU_FB_DEPTH_ATTACHMENT, GPU_FB_DEPTH_STENCIL_ATTACHMENT)) {
    if (depth_attachment().tex == nullptr) {
      return;
    }
    DWORD flags = D3DCLEAR_ZBUFFER;
    if (type == GPU_FB_DEPTH_STENCIL_ATTACHMENT) {
      flags |= D3DCLEAR_STENCIL;
    }
    clear_full_viewport(device, flags, 0, float(clear_value.x), 0);
    return;
  }

  const int slot = type - GPU_FB_COLOR_ATTACHMENT0;
  bool other_targets = false;
  for (int i = 0; i < D3D9_MAX_RENDER_TARGETS; i++) {
    if (i != slot && attachments_[GPU_FB_COLOR_ATTACHMENT0 + i].tex) {
      other_targets = true;
    }
  }
  if (!other_targets && (slot == 0 || is_window_)) {
    clear_full_viewport(device, D3DCLEAR_TARGET, to_d3d9_color(clear_value), 1.0f, 0);
    return;
  }
  /* Bind the attachment alone. */
  IDirect3DSurface9 *surface = color_surface(slot);
  if (surface == nullptr) {
    return;
  }
  const int max_targets = clamp_i(int(D3D9Backend::get()->caps.NumSimultaneousRTs), 1, 4);
  device->SetRenderTarget(0, surface);
  for (int i = 1; i < max_targets; i++) {
    device->SetRenderTarget(i, nullptr);
  }
  surface->Release();
  clear_full_viewport(device, D3DCLEAR_TARGET, to_d3d9_color(clear_value), 1.0f, 0);
  tag_dirty();
}

void D3D9FrameBuffer::clear_multi(Span<double4> clear_cols)
{
  for (int i = 0; i < D3D9_MAX_RENDER_TARGETS && i < clear_cols.size(); i++) {
    if (attachments_[GPU_FB_COLOR_ATTACHMENT0 + i].tex) {
      clear_attachment(GPU_FB_COLOR_ATTACHMENT0 + i, clear_cols[i]);
    }
  }
}

void D3D9FrameBuffer::attachment_set_loadstore_op(GPUAttachmentType type, GPULoadStore ls)
{
  if (ls.load_action == GPU_LOADACTION_CLEAR) {
    clear_attachment(type, double4(ls.clear_value[0],
                                   ls.clear_value[1],
                                   ls.clear_value[2],
                                   ls.clear_value[3]));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Read-back & blit
 * \{ */

void D3D9FrameBuffer::read(GPUFrameBufferBits planes,
                           eGPUDataFormat format,
                           const int area[4],
                           int channel_len,
                           int slot,
                           void *r_data)
{
  const size_t texel_size = size_t(channel_len) *
                            (format == GPU_DATA_UBYTE ? 1 :
                             format == GPU_DATA_HALF_FLOAT ? 2 :
                                                             4);
  const size_t size = texel_size * area[2] * area[3];

  if (planes != GPU_COLOR_BIT) {
    /* Depth can't be read back on SM2.0 hardware: report the far plane. */
    if (planes == GPU_DEPTH_BIT && format == GPU_DATA_FLOAT) {
      float *depth = static_cast<float *>(r_data);
      for (int i = 0; i < area[2] * area[3]; i++) {
        depth[i] = 1.0f;
      }
    }
    else {
      memset(r_data, 0, size);
    }
    return;
  }
  memset(r_data, 0, size);

  D3D9Context *ctx = D3D9Context::get();
  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  ctx->end_scene();

  IDirect3DSurface9 *surface = nullptr;
  D3D9TextureFormat device_format = {D3DFMT_A8R8G8B8, D3D9TexelKind::UNORM8, 4};
  if (is_window_) {
    surface = window_surface_;
    if (surface) {
      surface->AddRef();
    }
  }
  else {
    surface = color_surface(slot);
    device_format = static_cast<D3D9Texture *>(attachments_[GPU_FB_COLOR_ATTACHMENT0 + slot].tex)
                        ->device_format();
  }
  if (surface == nullptr) {
    return;
  }

  D3DSURFACE_DESC desc;
  surface->GetDesc(&desc);
  IDirect3DSurface9 *staging = nullptr;
  if (!D3D9_CHECK(device->CreateOffscreenPlainSurface(
          desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr)))
  {
    surface->Release();
    return;
  }
  d3d9_frame_stats.readbacks++;
  d3d9_frame_stats.last_readback = name_;
  const bool ok = D3D9_CHECK(device->GetRenderTargetData(surface, staging));
  surface->Release();

  D3DLOCKED_RECT locked;
  if (ok && SUCCEEDED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
    const int device_texel = device_format.kind == D3D9TexelKind::PACKED_U32 ? 4 :
                             device_format.kind == D3D9TexelKind::UNORM8 ?
                                 (device_format.components == 1 ? 1 : 4) :
                             device_format.kind == D3D9TexelKind::HALF ?
                                 device_format.components * 2 :
                                 device_format.components * 4;
    uint8_t *dst = static_cast<uint8_t *>(r_data);
    for (int row = 0; row < area[3]; row++) {
      const int gl_y = area[1] + row;
      const int device_y = is_flipped() ? gl_y : int(desc.Height) - 1 - gl_y;
      if (device_y < 0 || device_y >= int(desc.Height)) {
        dst += texel_size * area[2];
        continue;
      }
      const uint8_t *src = static_cast<const uint8_t *>(locked.pBits) + device_y * locked.Pitch;
      for (int col = 0; col < area[2]; col++, dst += texel_size) {
        const int x = area[0] + col;
        if (x < 0 || x >= int(desc.Width)) {
          continue;
        }
        const uint8_t *s = src + x * device_texel;
        if (device_format.kind == D3D9TexelKind::PACKED_U32 &&
            ELEM(format, GPU_DATA_UINT, GPU_DATA_INT))
        {
          /* Exact integers, beyond the 24 bits of a float. */
          memcpy(dst, s, 4);
          continue;
        }
        float texel[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (device_format.kind == D3D9TexelKind::UNORM8 && device_format.components == 4) {
          texel[0] = s[2] / 255.0f;
          texel[1] = s[1] / 255.0f;
          texel[2] = s[0] / 255.0f;
          texel[3] = s[3] / 255.0f;
        }
        else if (device_format.kind == D3D9TexelKind::UNORM8) {
          texel[0] = s[0] / 255.0f;
        }
        else if (device_format.kind == D3D9TexelKind::HALF) {
          const uint16_t *h = reinterpret_cast<const uint16_t *>(s);
          for (int c = 0; c < device_format.components; c++) {
            texel[c] = math::half_to_float(h[c]);
          }
        }
        else if (device_format.kind == D3D9TexelKind::PACKED_U32) {
          uint32_t v;
          memcpy(&v, s, 4);
          texel[0] = float(v);
        }
        else {
          memcpy(texel, s, sizeof(float) * device_format.components);
        }
        for (int c = 0; c < channel_len && c < 4; c++) {
          switch (format) {
            case GPU_DATA_UBYTE:
              dst[c] = uint8_t(clamp_f(texel[c], 0.0f, 1.0f) * 255.0f + 0.5f);
              break;
            case GPU_DATA_FLOAT:
              reinterpret_cast<float *>(dst)[c] = texel[c];
              break;
            case GPU_DATA_HALF_FLOAT:
              reinterpret_cast<uint16_t *>(dst)[c] = math::float_to_half(texel[c]);
              break;
            case GPU_DATA_UINT:
              reinterpret_cast<uint32_t *>(dst)[c] = uint32_t(max_ff(texel[c], 0.0f));
              break;
            case GPU_DATA_INT:
              reinterpret_cast<int32_t *>(dst)[c] = int32_t(texel[c]);
              break;
            default:
              break;
          }
        }
      }
    }
    staging->UnlockRect();
  }
  staging->Release();
}

void D3D9FrameBuffer::blit_to(GPUFrameBufferBits planes,
                              int src_slot,
                              FrameBuffer *dst_,
                              int dst_slot,
                              int dst_offset_x,
                              int dst_offset_y)
{
  D3D9FrameBuffer *dst = static_cast<D3D9FrameBuffer *>(dst_);
  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  D3D9Context *ctx = D3D9Context::get();
  if (dirty_attachments_) {
    update_attachments();
  }
  if (dst->dirty_attachments_) {
    dst->update_attachments();
  }
  if (is_flipped() != dst->is_flipped()) {
    /* StretchRect can't flip: draw the color attachment instead. */
    D3D9Texture *src_tex = static_cast<D3D9Texture *>(
        attachments_[GPU_FB_COLOR_ATTACHMENT0 + src_slot].tex);
    if ((planes & GPU_COLOR_BIT) && src_tex && !is_window_) {
      ctx->active_fb = dst;
      ctx->state_manager_get().active_fb = dst;
      dst->tag_dirty();
      dst->apply_state();
      int rect[4] = {dst_offset_x, dst_offset_y, width_, height_};
      RECT dst_rect;
      dst->to_device_rect(rect, dst_rect);
      ctx->ensure_scene();
      D3D9Backend::get()->draw_texture_rect(src_tex->base_texture_get(), dst_rect, true);
      ctx->state_invalidate();
    }
    else {
      CLOG_WARN(&LOG, "Blit between frame-buffers of different orientation is not supported");
    }
    return;
  }
  ctx->end_scene();

  if (planes & GPU_COLOR_BIT) {
    IDirect3DSurface9 *src_surface = is_window_ ? window_surface_ : color_surface(src_slot);
    IDirect3DSurface9 *dst_surface = dst->is_window_ ? dst->window_surface_ :
                                                       dst->color_surface(dst_slot);
    if (is_window_ && src_surface) {
      src_surface->AddRef();
    }
    if (dst->is_window_ && dst_surface) {
      dst_surface->AddRef();
    }
    if (src_surface && dst_surface) {
      int rect[4] = {dst_offset_x, dst_offset_y, width_, height_};
      RECT dst_rect;
      dst->to_device_rect(rect, dst_rect);
      RECT src_rect = {0, 0, dst_rect.right - dst_rect.left, dst_rect.bottom - dst_rect.top};
      if (!is_flipped()) {
        src_rect.top = height_ - src_rect.bottom;
        src_rect.bottom = height_;
      }
      D3D9_CHECK(
          device->StretchRect(src_surface, &src_rect, dst_surface, &dst_rect, D3DTEXF_NONE));
    }
    d3d9_release(src_surface);
    d3d9_release(dst_surface);
  }
  if (planes & (GPU_DEPTH_BIT | GPU_STENCIL_BIT)) {
    IDirect3DSurface9 *src_depth = depth_surface();
    IDirect3DSurface9 *dst_depth = dst->depth_surface();
    if (src_depth && dst_depth) {
      /* Depth stretch is only allowed for whole surfaces. */
      D3D9_CHECK(device->StretchRect(src_depth, nullptr, dst_depth, nullptr, D3DTEXF_NONE));
    }
    d3d9_release(src_depth);
    d3d9_release(dst_depth);
  }
  /* The destination is now the active frame-buffer (like OpenGL). */
  ctx->active_fb = dst;
  ctx->state_manager_get().active_fb = dst;
  dst->tag_dirty();
}

/** \} */

}  // namespace blender::gpu
