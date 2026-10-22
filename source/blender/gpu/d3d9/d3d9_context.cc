/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstdio>

#include "BLI_hash.hh"

#include "GHOST_IWindow.hh"

#include "d3d9_backend.hh"
#include "d3d9_context.hh"
#include "d3d9_framebuffer.hh"
#include "d3d9_immediate.hh"
#include "d3d9_shader.hh"
#include "d3d9_state.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Vertex declarations
 * \{ */

uint64_t D3D9VertexDeclKey::hash() const
{
  uint64_t hash = elements.size();
  for (const D3DVERTEXELEMENT9 &e : elements) {
    hash = hash * 33 ^ (uint64_t(e.Stream) | uint64_t(e.Offset) << 8 | uint64_t(e.Type) << 24 |
                        uint64_t(e.UsageIndex) << 32);
  }
  return hash;
}

bool D3D9VertexDeclKey::operator==(const D3D9VertexDeclKey &other) const
{
  if (elements.size() != other.elements.size()) {
    return false;
  }
  return memcmp(elements.data(),
                other.elements.data(),
                elements.size() * sizeof(D3DVERTEXELEMENT9)) == 0;
}

IDirect3DVertexDeclaration9 *D3D9Context::vertex_declaration_get(const D3D9VertexDeclKey &key)
{
  return vertex_decls_.lookup_or_add_cb(key, [&]() -> IDirect3DVertexDeclaration9 * {
    Vector<D3DVERTEXELEMENT9, 9> elements(key.elements);
    elements.append(D3DDECL_END());
    IDirect3DVertexDeclaration9 *decl = nullptr;
    D3D9_CHECK(device->CreateVertexDeclaration(elements.data(), &decl));
    return decl;
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Constructor / Destructor
 * \{ */

D3D9Context::D3D9Context(GHOST_IWindow *ghost_window, GHOST_IContext *ghost_context)
{
  if (ghost_context == nullptr && ghost_window != nullptr) {
    ghost_context = ghost_window->getDrawingContext();
  }
  ghost_context_ = ghost_context;
  ghost_window_ = ghost_window;

  GHOST_D3D9Handles handles = {};
  if (ghost_context_) {
    ghost_context_->getD3D9Handles(handles);
  }
  device = static_cast<IDirect3DDevice9 *>(handles.device);
  BLI_assert_msg(device, "D3D9 context created without a D3D9 GHOST context");

  D3D9Backend *backend = D3D9Backend::get();
  if (backend->device == nullptr) {
    backend->device = device;
  }

  state_manager = new D3D9StateManager(device);
  imm = new D3D9Immediate();

  D3D9FrameBuffer *window_fb = new D3D9FrameBuffer("back_left", this);
  window_fb->window_surface_set(static_cast<IDirect3DSurface9 *>(handles.back_buffer),
                                handles.width,
                                handles.height);
  back_left = window_fb;
  active_fb = back_left;
  state_manager_get().active_fb = window_fb;
}

D3D9Context::~D3D9Context()
{
  free_resources();
  end_scene();
  for (IDirect3DVertexDeclaration9 *decl : vertex_decls_.values()) {
    if (decl) {
      decl->Release();
    }
  }
  vertex_decls_.clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Activation
 * \{ */

void D3D9Context::window_framebuffer_update()
{
  if (ghost_context_ == nullptr || ghost_window_ == nullptr) {
    return;
  }
  GHOST_D3D9Handles handles = {};
  ghost_context_->getD3D9Handles(handles);
  static_cast<D3D9FrameBuffer *>(back_left)
      ->window_surface_set(
          static_cast<IDirect3DSurface9 *>(handles.back_buffer), handles.width, handles.height);
}

void D3D9Context::activate()
{
  BLI_assert(is_active_ == false);
  is_active_ = true;
  thread_ = pthread_self();

  window_framebuffer_update();

  /* Another context may have used the shared device: send our state again. */
  static_cast<D3D9FrameBuffer *>(active_fb)->tag_dirty();
  state_manager->force_state();

  immActivate();
}

void D3D9Context::deactivate()
{
  immDeactivate();
  end_scene();
  is_active_ = false;
}

void D3D9Context::begin_frame()
{
  window_framebuffer_update();
}

void D3D9Context::end_frame()
{
  /* `Present` must be called outside of a scene. */
  end_scene();
}

void D3D9Context::flush()
{
  end_scene();
}

void D3D9Context::finish()
{
  end_scene();
  IDirect3DQuery9 *query = nullptr;
  if (SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_EVENT, &query))) {
    query->Issue(D3DISSUE_END);
    while (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
      Sleep(0);
    }
    query->Release();
  }
}

void D3D9Context::memory_statistics_get(int *r_total_mem, int *r_free_mem)
{
  /* Only the available texture memory is known (in bytes). */
  const int free_kb = int(device->GetAvailableTextureMem() / 1024);
  *r_total_mem = free_kb;
  *r_free_mem = free_kb;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw Setup
 * \{ */

bool D3D9Context::draw_setup()
{
  D3D9Shader *sh = static_cast<D3D9Shader *>(shader);
  if (sh == nullptr || !sh->is_valid()) {
    return false;
  }

  D3D9FrameBuffer *fb = static_cast<D3D9FrameBuffer *>(active_fb);
  D3D9StateManager &state = state_manager_get();
  if (state.active_fb != fb) {
    state.active_fb = fb;
  }
  fb->apply_state();
  state.apply_state();

  /* Reserved vertex shader constants, see `d3d9_common.hlsl`. */
  int viewport[4];
  fb->viewport_get(viewport);
  const bool unit_depth_range = state.state.clip_control;
  reserved_constants_[0] = float4(fb->is_flipped() ? -1.0f : 1.0f,
                                  unit_depth_range ? 1.0f : 0.5f,
                                  unit_depth_range ? 0.0f : 0.5f,
                                  0.0f);
  reserved_constants_[1] = float4(
      -1.0f / float(max_ii(viewport[2], 1)), 1.0f / float(max_ii(viewport[3], 1)), 0.0f, 0.0f);
  device->SetVertexShaderConstantF(0, &reserved_constants_[0].x, D3D9_RESERVED_VS_CONSTANTS);

  static const bool trace = getenv("BLENDER_D3D9_TRACE") != nullptr;
  if (trace) {
    const float4 *ps = sh->ps_constants_get();
    printf("draw %s fb=%s vp=(%d %d %d %d) blend=%d ps0=(%.3f %.3f %.3f %.3f)\n",
           sh->name_get().c_str(),
           fb->name_get(),
           UNPACK4(viewport),
           int(state.state.blend),
           ps ? ps->x : -1.0f,
           ps ? ps->y : -1.0f,
           ps ? ps->z : -1.0f,
           ps ? ps->w : -1.0f);
  }

  sh->constants_upload(device);

  ensure_scene();
  return true;
}

void D3D9Context::state_invalidate()
{
  static_cast<D3D9FrameBuffer *>(active_fb)->tag_dirty();
  state_manager->force_state();
  if (shader) {
    shader->bind(nullptr);
  }
}

void D3D9Context::instance_id_set(int instance)
{
  reserved_constants_[0].w = float(instance);
  device->SetVertexShaderConstantF(0, &reserved_constants_[0].x, 1);
}

/** \} */

}  // namespace blender::gpu
