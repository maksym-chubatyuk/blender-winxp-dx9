/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_framebuffer_private.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

class D3D9Context;

/** SM2.0 hardware has at most 4 simultaneous render targets. */
constexpr int D3D9_MAX_RENDER_TARGETS = 4;

class D3D9FrameBuffer : public FrameBuffer {
 public:
  /** Frame-buffer with texture attachments. */
  explicit D3D9FrameBuffer(const char *name);
  /** Window frame-buffer, rendering into the swap chain back buffer. */
  D3D9FrameBuffer(const char *name, D3D9Context *context);
  ~D3D9FrameBuffer() override;

  void bind(bool enabled_srgb) override;
  bool check(char err_out[256]) override;
  void clear(GPUFrameBufferBits buffers,
             const double4 clear_color,
             float clear_depth,
             uint clear_stencil) override;
  void clear_multi(Span<double4> clear_cols) override;
  void clear_attachment(GPUAttachmentType type, const double4 clear_value) override;
  void attachment_set_loadstore_op(GPUAttachmentType type, GPULoadStore ls) override;
  void read(GPUFrameBufferBits planes,
            eGPUDataFormat format,
            const int area[4],
            int channel_len,
            int slot,
            void *r_data) override;
  void blit_to(GPUFrameBufferBits planes,
               int src_slot,
               FrameBuffer *dst,
               int dst_slot,
               int dst_offset_x,
               int dst_offset_y) override;

  /** Set the swap chain back buffer of a window frame-buffer. */
  void window_surface_set(IDirect3DSurface9 *surface, int width, int height);

  /**
   * Off-screen targets are rendered upside-down to keep the OpenGL memory layout.
   * Window frame-buffers are not.
   */
  bool is_flipped() const
  {
    return !is_window_;
  }

  /**
   * Bind the render targets on the device if they changed, then viewport and scissor.
   * \param blending: The draw blends. Without MRT blending support only the first color target
   * is bound then (the others only receive line data in the overlays, which isn't written).
   */
  void apply_state(bool blending = false);
  /** Force the next #apply_state to bind everything again. */
  void tag_dirty()
  {
    dirty_targets_ = true;
    dirty_state_ = true;
  }

 protected:
  void subpass_transition_impl(const GPUAttachmentState /*depth_attachment_state*/,
                               Span<GPUAttachmentState> /*color_attachment_states*/) override
  {
  }

 private:
  void update_attachments();
  /** Viewport/scissor rectangle in device coordinates (top-left origin). */
  void to_device_rect(const int rect[4], RECT &r_rect) const;
  IDirect3DSurface9 *color_surface(int slot);
  IDirect3DSurface9 *depth_surface();

  D3D9Context *context_ = nullptr;
  bool is_window_ = false;
  IDirect3DSurface9 *window_surface_ = nullptr;
  bool dirty_targets_ = true;
  /** Only the first color target is bound, see #apply_state. */
  bool single_target_ = false;
  bool srgb_ = false;
  bool enabled_srgb_ = false;
};

}  // namespace blender::gpu
