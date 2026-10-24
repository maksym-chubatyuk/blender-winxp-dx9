/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_map.hh"

#include "gpu_context_private.hh"

#include "GHOST_IContext.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

class D3D9FrameBuffer;
class D3D9StateManager;
class D3D9Shader;
class D3D9UniformBuf;
class D3D9StorageBuf;

/** Maximum storage buffer bindings tracked. */
constexpr int D3D9_MAX_SSBO_SLOTS = 16;

/** Maximum uniform buffer bindings tracked. */
constexpr int D3D9_MAX_UBO_SLOTS = 16;

/** Key of a vertex declaration: the packed `D3DVERTEXELEMENT9` list. */
struct D3D9VertexDeclKey {
  Vector<D3DVERTEXELEMENT9, 8> elements;

  uint64_t hash() const;
  bool operator==(const D3D9VertexDeclKey &other) const;
};

/** Work counters for `BLENDER_D3D9_FRAMETIME`. */
struct D3D9FrameStats {
  int draws = 0;
  int readbacks = 0;
  size_t upload_bytes = 0;
  const char *last_readback = "";
  /* Time spent in the backend per part (ms), see #d3d9_time_ms. */
  double setup_ms = 0.0;
  double draw_ms = 0.0;
  double clear_ms = 0.0;
  /* Parts of the setup. */
  double fb_ms = 0.0;
  double state_ms = 0.0;
  double texture_ms = 0.0;
  double constants_ms = 0.0;
  double scene_ms = 0.0;
  int textures_created = 0;
  int textures_freed = 0;
};
/** Milliseconds from an arbitrary origin (performance counter). */
double d3d9_time_ms();

/**
 * `BLENDER_D3D9_GPUTIME`: GPU time of each draw, measured by waiting for the GPU before and after
 * it (slow, but it isolates the draws). Summed per shader and printed with the frame times.
 */
bool d3d9_gpu_time_enabled();
/** Wait until the GPU is idle and return the time. */
double d3d9_gpu_time_begin(IDirect3DDevice9 *device);
/** Wait until the GPU is done and add the time since #d3d9_gpu_time_begin to the name. */
void d3d9_gpu_time_end(IDirect3DDevice9 *device, const char *name, double start_ms);
extern D3D9FrameStats d3d9_frame_stats;

class D3D9Context : public Context {
 public:
  IDirect3DDevice9 *device = nullptr;
  /** Uniform buffers bound with #GPU_uniformbuf_bind. */
  D3D9UniformBuf *bound_ubos[D3D9_MAX_UBO_SLOTS] = {};
  /** Storage buffers bound with #GPU_storagebuf_bind, their data is read by the CPU. */
  D3D9StorageBuf *bound_ssbos[D3D9_MAX_SSBO_SLOTS] = {};

  D3D9Context(GHOST_IWindow *ghost_window, GHOST_IContext *ghost_context);
  ~D3D9Context() override;

  static D3D9Context *get()
  {
    return static_cast<D3D9Context *>(Context::get());
  }

  void activate() override;
  void deactivate() override;
  void begin_frame() override;
  void end_frame() override;

  void flush() override;
  void finish() override;

  void memory_statistics_get(int *r_total_mem, int *r_free_mem) override;
  int64_t render_target_budget() override;
  double texture_write_rate(Texture *texture) override;
  void memory_evict_managed() override;

  bool debug_capture_begin(const char * /*title*/) override
  {
    return false;
  }
  void debug_capture_end() override {}
  void *debug_capture_scope_create(const char * /*name*/) override
  {
    return nullptr;
  }
  bool debug_capture_scope_begin(void * /*scope*/) override
  {
    return false;
  }
  void debug_capture_scope_end(void * /*scope*/) override {}
  void debug_unbind_all_ubo() override {}
  void debug_unbind_all_ssbo() override {}

  D3D9StateManager &state_manager_get()
  {
    return *reinterpret_cast<D3D9StateManager *>(state_manager);
  }

  /** Rendering commands must be inside a scene, opened lazily and closed by #end_scene. */
  void ensure_scene()
  {
    if (!scene_open_) {
      device->BeginScene();
      scene_open_ = true;
    }
  }
  void end_scene()
  {
    if (scene_open_) {
      device->EndScene();
      scene_open_ = false;
    }
  }

  /**
   * Make everything ready for a draw call with the bound shader: pipeline state, frame-buffer,
   * textures and shader constants. Returns false when the draw has to be skipped.
   */
  bool draw_setup();
  bool draw_setup_impl();

  /** Set `gl_InstanceID` for the next draw (reserved constant `c0.w`). */
  void instance_id_set(int instance);

  /** Rasterize the points of the next draws as sprites (see #D3D9Shader::point_sprite). */
  void point_sprite_set(bool enable);

  /** Flat shading of the `COLOR` outputs for the next draws (see #D3D9Shader::flat_color). */
  void flat_color_set(bool enable);

  /**
   * Selection draws (`d3d9-select-output` shaders): the fragments write (select id + 1, depth) to
   * a private float target instead of the frame-buffer colors. #select_draw_end reads it back and
   * applies `select_id_output()` (`select_lib.glsl`) on the CPU to the bound selection buffers.
   */
  bool select_draw_begin();
  void select_draw_end();

  /** Get or create the vertex declaration for the element list. */
  IDirect3DVertexDeclaration9 *vertex_declaration_get(const D3D9VertexDeclKey &key);

  /** Device state was changed behind the state manager: send everything again before drawing. */
  void state_invalidate();

  /** Update the window frame-buffer from the GHOST swap chain (size and surface). */
  void window_framebuffer_update();

 private:
  GHOST_IContext *ghost_context_ = nullptr;
  bool scene_open_ = false;
  double frame_start_ms_ = 0.0;
  /** Last reserved vertex constants sent by #draw_setup. */
  float4 reserved_constants_[D3D9_RESERVED_VS_CONSTANTS];
  /** Last #D3DRS_POINTSPRITEENABLE value, -1 when unknown. */
  int point_sprite_ = -1;
  /** Last #D3DRS_SHADEMODE (1 for flat), -1 when unknown. */
  int flat_color_ = -1;
  Map<D3D9VertexDeclKey, IDirect3DVertexDeclaration9 *> vertex_decls_;

  /* Selection draws, see #select_draw_begin. */
  IDirect3DTexture9 *select_tx_ = nullptr;
  IDirect3DSurface9 *select_readback_ = nullptr;
  int select_width_ = 0, select_height_ = 0;
  DWORD select_saved_states_[3] = {};
};

}  // namespace blender::gpu
