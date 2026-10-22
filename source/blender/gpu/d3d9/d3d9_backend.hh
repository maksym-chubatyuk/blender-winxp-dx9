/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "gpu_backend.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

class D3D9Context;

class D3D9Backend : public GPUBackend {
 public:
  /** Device shared by all contexts, set when the first context is created. */
  IDirect3DDevice9 *device = nullptr;
  D3DCAPS9 caps = {};
  /** Display format of the adapter (the back buffer format). */
  D3DFORMAT display_format = D3DFMT_X8R8G8B8;

  /** Optional features, see #capabilities_init. */
  bool supports_separate_alpha_blend = false;
  bool supports_non_pow2 = false;
  bool supports_index_u32 = false;
  bool supports_autogen_mipmap = false;
  bool supports_blend_op = false;
  bool supports_scissor = false;

  D3D9Backend();
  ~D3D9Backend() override;

  static bool is_supported();
  static D3D9Backend *get()
  {
    return static_cast<D3D9Backend *>(GPUBackend::get());
  }

  /** Can the format be used for textures with the given usage (`D3DUSAGE_*`). */
  bool format_supported(D3DFORMAT format, DWORD usage, D3DRESOURCETYPE type) const;

  void init_resources() override;
  void delete_resources() override;

  void compute_dispatch(int groups_x_len, int groups_y_len, int groups_z_len) override;
  void compute_dispatch_indirect(StorageBuf *indirect_buf) override;

  Context *context_alloc(GHOST_IWindow *ghost_window, GHOST_IContext *ghost_context) override;

  Batch *batch_alloc() override;
  Fence *fence_alloc() override;
  FrameBuffer *framebuffer_alloc(const char *name) override;
  IndexBuf *indexbuf_alloc() override;
  PixelBuffer *pixelbuf_alloc(size_t size) override;
  QueryPool *querypool_alloc() override;
  Shader *shader_alloc(const char *name) override;
  Texture *texture_alloc(const char *name) override;
  TexturePool *texturepool_alloc() override;
  UniformBuf *uniformbuf_alloc(size_t size, const char *name) override;
  StorageBuf *storagebuf_alloc(size_t size, GPUUsageType usage, const char *name) override;
  VertBuf *vertbuf_alloc() override;
  void shader_cache_dir_clear_old() override {}

  void render_begin() override {}
  void render_end() override {}
  void render_step(bool /*force_resource_release*/) override {}

  /* -------------------------------------------------------------------- */
  /** \name Shared device resources
   * \{ */

  /** Static vertex buffer of `float` vertex indices, bound for shaders using `TEXCOORD15`. */
  IDirect3DVertexBuffer9 *vertex_id_buffer();
  /**
   * Index buffer of `(i, 0)` index pairs: drawing pair `n - 1` as a line list closes a line loop of
   * `n` vertices (the strip part is drawn with #D3DPT_LINESTRIP).
   */
  IDirect3DIndexBuffer9 *line_loop_index_buffer();
  /** Static vertex buffer of white `D3DCOLOR`, bound for vertex shader inputs without data. */
  IDirect3DVertexBuffer9 *default_attr_buffer();
  /**
   * D3D9 needs a color target bound at all times, depth only frame-buffers get a dummy one.
   * The returned surface is shared, nothing should read it.
   */
  IDirect3DSurface9 *dummy_color_target(int width, int height);

  /**
   * Draw a texture over a rectangle of the bound render target (device coordinates), ignoring
   * the pipeline state. Used for the blits StretchRect can't do (vertical flip).
   * The caller has to restore the state (#D3D9Context::state_invalidate).
   */
  void draw_texture_rect(IDirect3DBaseTexture9 *texture, const RECT &dst_rect, bool flip_v);

  /** \} */

 private:
  void platform_init();
  void capabilities_init();

  IDirect3D9 *d3d_ = nullptr;
  IDirect3DVertexBuffer9 *vertex_id_buffer_ = nullptr;
  IDirect3DIndexBuffer9 *line_loop_ibo_ = nullptr;
  IDirect3DVertexBuffer9 *default_attr_buffer_ = nullptr;
  IDirect3DSurface9 *dummy_color_ = nullptr;
  IDirect3DVertexShader9 *blit_vs_ = nullptr;
  IDirect3DPixelShader9 *blit_ps_ = nullptr;
  IDirect3DVertexDeclaration9 *blit_decl_ = nullptr;
  int dummy_color_size_[2] = {0, 0};
};

}  // namespace blender::gpu
