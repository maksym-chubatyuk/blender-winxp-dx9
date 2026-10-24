/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>

#include "BLI_hash.hh"
#include "BLI_math_base.h"
#include "BLI_math_bits.h"

#include "GHOST_IWindow.hh"

#include "d3d9_backend.hh"
#include "d3d9_buffers.hh"
#include "d3d9_context.hh"
#include "d3d9_framebuffer.hh"
#include "d3d9_immediate.hh"
#include "d3d9_shader.hh"
#include "d3d9_state.hh"
#include "d3d9_texture.hh"

#include <ddraw.h>

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
  /* The window has no separate front buffer: reads of it (the file thumbnail on save) get the
   * back buffer, which holds the last presented frame. Aliased, see the destructor. */
  front_left = window_fb;
  active_fb = back_left;
  state_manager_get().active_fb = window_fb;
}

D3D9Context::~D3D9Context()
{
  /* Aliases `back_left`, freed once. */
  front_left = nullptr;
  free_resources();
  end_scene();
  for (IDirect3DVertexDeclaration9 *decl : vertex_decls_.values()) {
    if (decl) {
      decl->Release();
    }
  }
  vertex_decls_.clear();
  d3d9_release(select_tx_);
  d3d9_release(select_readback_);
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

D3D9FrameStats d3d9_frame_stats;

double d3d9_time_ms()
{
  static LARGE_INTEGER freq = {};
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

bool d3d9_gpu_time_enabled()
{
  static const bool enabled = getenv("BLENDER_D3D9_GPUTIME") != nullptr;
  return enabled;
}

static Map<std::string, double> &gpu_times()
{
  static Map<std::string, double> times;
  return times;
}

static void gpu_wait_idle(IDirect3DDevice9 *device)
{
  static IDirect3DQuery9 *query = nullptr;
  if (query == nullptr && FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &query))) {
    query = nullptr;
    return;
  }
  query->Issue(D3DISSUE_END);
  while (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
  }
}

double d3d9_gpu_time_begin(IDirect3DDevice9 *device)
{
  gpu_wait_idle(device);
  return d3d9_time_ms();
}

void d3d9_gpu_time_end(IDirect3DDevice9 *device, const char *name, const double start_ms)
{
  gpu_wait_idle(device);
  gpu_times().lookup_or_add(name, 0.0) += d3d9_time_ms() - start_ms;
}

static void gpu_times_print(int frames)
{
  Vector<std::pair<double, std::string>> sorted;
  double total = 0.0;
  for (const auto item : gpu_times().items()) {
    sorted.append({item.value, item.key});
    total += item.value;
  }
  std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
    return a.first > b.first;
  });
  printf("d3d9: GPU %.2f ms per frame:", total / frames);
  for (int i = 0; i < min_ii(sorted.size(), 14); i++) {
    printf(" %s %.2f,", sorted[i].second.c_str(), sorted[i].first / frames);
  }
  printf("\n");
  gpu_times().clear();
}

void D3D9Context::begin_frame()
{
  window_framebuffer_update();
  frame_start_ms_ = d3d9_time_ms();
}

void D3D9Context::end_frame()
{
  D3D9_HEAP_CHECK_SCOPE("D3D9Context::end_frame");
  /* `Present` must be called outside of a scene. */
  end_scene();
  D3D9Backend::get()->texture_cache_frame();

  /* `BLENDER_D3D9_FRAMETIME`: print averages of the frame CPU time and work. */
  static const bool frametime = getenv("BLENDER_D3D9_FRAMETIME") != nullptr;
  if (frametime) {
    static double cpu_sum = 0.0;
    static int frames = 0;
    cpu_sum += d3d9_time_ms() - frame_start_ms_;
    if (++frames == 32) {
      printf("d3d9: frame cpu %.2f ms (setup %.2f, draw calls %.2f, clears %.2f; fb %.2f, state "
             "%.2f, textures %.2f, constants %.2f, scene %.2f), %.1f draws, "
             "%.1f readbacks (%s), %.1f KB uploads per frame\n",
             cpu_sum / frames,
             d3d9_frame_stats.setup_ms / frames,
             d3d9_frame_stats.draw_ms / frames,
             d3d9_frame_stats.clear_ms / frames,
             d3d9_frame_stats.fb_ms / frames,
             d3d9_frame_stats.state_ms / frames,
             d3d9_frame_stats.texture_ms / frames,
             d3d9_frame_stats.constants_ms / frames,
             d3d9_frame_stats.scene_ms / frames,
             float(d3d9_frame_stats.draws) / frames,
             float(d3d9_frame_stats.readbacks) / frames,
             d3d9_frame_stats.last_readback,
             float(d3d9_frame_stats.upload_bytes) / 1024.0f / frames);
      if (d3d9_gpu_time_enabled()) {
        gpu_times_print(frames);
      }
      printf("d3d9: %.1f textures created, %.1f freed per frame, %u MB texture memory available, "
             "%.1f MB of render targets\n",
             float(d3d9_frame_stats.textures_created) / frames,
             float(d3d9_frame_stats.textures_freed) / frames,
             device->GetAvailableTextureMem() / (1024 * 1024),
             double(d3d9_render_target_bytes()) / (1024.0 * 1024.0));
      fflush(stdout);
      cpu_sum = 0.0;
      frames = 0;
      d3d9_frame_stats = {};
    }
  }
}

void D3D9Context::flush()
{
  D3D9_HEAP_CHECK_SCOPE_DRAW("D3D9Context::flush");
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

/**
 * Local video memory of the adapter in bytes (DirectDraw 7 tells local and AGP memory apart, the
 * Direct3D 9 estimate adds them up), 0 when unknown. `BLENDER_D3D9_VRAM_MB` overrides it.
 */
static int64_t d3d9_local_video_memory()
{
  static int64_t bytes = -1;
  if (bytes >= 0) {
    return bytes;
  }
  bytes = 0;
  if (const char *override_mb = getenv("BLENDER_D3D9_VRAM_MB")) {
    bytes = int64_t(atoi(override_mb)) << 20;
    return bytes;
  }
  HMODULE ddraw = LoadLibraryA("ddraw.dll");
  if (ddraw == nullptr) {
    return bytes;
  }
  using DirectDrawCreateExFn = HRESULT(WINAPI *)(GUID *, LPVOID *, REFIID, IUnknown *);
  auto create = reinterpret_cast<DirectDrawCreateExFn>(
      reinterpret_cast<void *>(GetProcAddress(ddraw, "DirectDrawCreateEx")));
  IDirectDraw7 *dd = nullptr;
  /* `IID_IDirectDraw7`, defined here to not link `dxguid`. */
  static const GUID iid_directdraw7 = {
      0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
  if (create &&
      SUCCEEDED(create(nullptr, reinterpret_cast<LPVOID *>(&dd), iid_directdraw7, nullptr)))
  {
    DDSCAPS2 caps = {};
    caps.dwCaps = DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    DWORD total = 0, free_bytes = 0;
    if (SUCCEEDED(dd->GetAvailableVidMem(&caps, &total, &free_bytes))) {
      bytes = int64_t(total);
    }
    dd->Release();
  }
  FreeLibrary(ddraw);
  return bytes;
}

int64_t D3D9Context::render_target_budget()
{
  const int64_t local = d3d9_local_video_memory();
  if (local <= 0) {
    return -1;
  }
  /* The driver doesn't report what is free (it places render targets beyond the card's memory in
   * AGP memory instead of failing): subtract the desktop surface, a window sized swap chain, the
   * render targets allocated by the backend and a reserve for the driver. */
  D3DDISPLAYMODE mode = {};
  int64_t display = 0;
  if (SUCCEEDED(device->GetDisplayMode(0, &mode))) {
    display = int64_t(mode.Width) * mode.Height * 4;
  }
  const int64_t reserve = int64_t(8) << 20;
  return local - 2 * display - d3d9_render_target_bytes() - reserve;
}

double D3D9Context::texture_write_rate(Texture *texture)
{
  D3D9Texture *tex = static_cast<D3D9Texture *>(texture);
  if (tex == nullptr || tex->is_depth()) {
    return 0.0;
  }
  IDirect3DSurface9 *surface = tex->surface_get(0, 0);
  if (surface == nullptr) {
    return 0.0;
  }
  D3DSURFACE_DESC desc;
  surface->GetDesc(&desc);
  int texel_bytes = 4;
  switch (desc.Format) {
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F:
      texel_bytes = 8;
      break;
    case D3DFMT_A32B32G32R32F:
      texel_bytes = 16;
      break;
    case D3DFMT_R16F:
    case D3DFMT_L8:
      texel_bytes = desc.Format == D3DFMT_L8 ? 1 : 2;
      break;
    default:
      break;
  }
  const double bytes = double(desc.Width) * desc.Height * texel_bytes;
  /* Fill it repeatedly (a fixed function clear, no state to restore) until enough time passed for
   * the timer resolution, best of a few tries. */
  end_scene();
  IDirect3DQuery9 *query = nullptr;
  if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &query))) {
    surface->Release();
    return 0.0;
  }
  auto wait = [&]() {
    query->Issue(D3DISSUE_END);
    while (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
    }
  };
  wait();
  const int fills = max_ii(4, int(64.0 * 1048576.0 / bytes));
  double best = 0.0;
  for (int attempt = 0; attempt < 3; attempt++) {
    const double start = d3d9_time_ms();
    for (int i = 0; i < fills; i++) {
      device->ColorFill(surface, nullptr, D3DCOLOR_ARGB(0, i, 0, 0));
    }
    wait();
    const double ms = d3d9_time_ms() - start;
    if (ms > 0.0) {
      best = max_dd(best, bytes * fills / (ms * 1e-3));
    }
  }
  query->Release();
  surface->Release();
  return best;
}

void D3D9Context::memory_evict_managed()
{
  end_scene();
  device->EvictManagedResources();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw Setup
 * \{ */

bool D3D9Context::draw_setup()
{
  const double start_ms = d3d9_time_ms();
  const bool result = draw_setup_impl();
  d3d9_frame_stats.setup_ms += d3d9_time_ms() - start_ms;
  return result;
}

bool D3D9Context::draw_setup_impl()
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
  d3d9_frame_stats.draws++;
  double t = d3d9_time_ms();
  fb->apply_state(state.state.blend != GPU_BLEND_NONE,
                  (state.state.write_mask & GPU_WRITE_COLOR) == 0);
  d3d9_frame_stats.fb_ms += d3d9_time_ms() - t;
  t = d3d9_time_ms();
  state.apply_state();
  d3d9_frame_stats.state_ms += d3d9_time_ms() - t;
  /* `BLENDER_D3D9_ZALWAYS=<shader name>`: no depth test for the draws of the shader (debugging). */
  static const char *zalways = getenv("BLENDER_D3D9_ZALWAYS");
  if (zalways && sh->name_get() == zalways) {
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    /* Send the depth test of the next draw again. */
    state.depth_test_invalidate();
  }

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
    printf("draw %s fb=%s vp=(%d %d %d %d) blend=%d depth=%d write=%d cull=%d invert=%d "
           "flipped=%d stencil=%d ps0=(%.3f %.3f %.3f %.3f)\n",
           sh->name_get().c_str(),
           fb->name_get(),
           UNPACK4(viewport),
           int(state.state.blend),
           int(state.state.depth_test),
           int(state.state.write_mask),
           int(state.state.culling_test),
           int(state.state.invert_facing),
           int(fb->is_flipped()),
           int(state.state.stencil_test),
           ps ? ps->x : -1.0f,
           ps ? ps->y : -1.0f,
           ps ? ps->z : -1.0f,
           ps ? ps->w : -1.0f);
  }

  if (trace) {
    for (int unit = 0; unit < 4; unit++) {
      D3D9Texture *tex = state.texture_get(unit);
      if (tex) {
        printf("  tex%d %s %p rt=%d %dx%d\n",
               unit,
               tex->debug_name(),
               static_cast<void *>(tex->base_texture_get()),
               int(tex->is_render_target()),
               tex->device_width(),
               tex->device_height());
      }
    }
    for (int slot = 0; slot < 2; slot++) {
      gpu::Texture *tex = fb->color_tex(slot);
      if (tex) {
        printf("  rt%d %s %p\n",
               slot,
               static_cast<D3D9Texture *>(tex)->debug_name(),
               static_cast<void *>(static_cast<D3D9Texture *>(tex)->base_texture_get()));
      }
    }
  }

  /* `BLENDER_D3D9_TRACE_VS=<shader name>`: dump the vertex shader constants of its draws. */
  static const char *trace_vs = getenv("BLENDER_D3D9_TRACE_VS");
  if (trace_vs && sh->name_get() == trace_vs) {
    const Span<float4> vs = sh->vs_constants_get();
    for (const int i : vs.index_range()) {
      printf("  vs c%d = (%g %g %g %g)\n", i, vs[i].x, vs[i].y, vs[i].z, vs[i].w);
    }
  }

  t = d3d9_time_ms();
  sh->constants_upload(device);
  d3d9_frame_stats.constants_ms += d3d9_time_ms() - t;

  t = d3d9_time_ms();
  ensure_scene();
  d3d9_frame_stats.scene_ms += d3d9_time_ms() - t;
  return true;
}

void D3D9Context::state_invalidate()
{
  static_cast<D3D9FrameBuffer *>(active_fb)->tag_dirty();
  state_manager->force_state();
  point_sprite_ = -1;
  flat_color_ = -1;
  if (shader) {
    shader->bind(nullptr);
  }
}

/* Keep in sync with `select_shader_shared.hh` and `select_defines.hh`. */
constexpr int SELECT_DATA_SLOT = 4;
constexpr int SELECT_ID_OUT_SLOT = 6;
enum { SELECT_ALL = 0, SELECT_PICK_ALL = 1, SELECT_PICK_NEAREST = 2 };

bool D3D9Context::select_draw_begin()
{
  D3D9FrameBuffer *fb = static_cast<D3D9FrameBuffer *>(active_fb);
  const int2 size = fb->size_get();
  if (size.x <= 0 || size.y <= 0) {
    return false;
  }
  if (select_tx_ == nullptr || select_width_ != size.x || select_height_ != size.y) {
    d3d9_release(select_tx_);
    d3d9_release(select_readback_);
    if (!D3D9_CHECK(device->CreateTexture(UINT(size.x),
                                          UINT(size.y),
                                          1,
                                          D3DUSAGE_RENDERTARGET,
                                          D3DFMT_A32B32G32R32F,
                                          D3DPOOL_DEFAULT,
                                          &select_tx_,
                                          nullptr)) ||
        !D3D9_CHECK(device->CreateOffscreenPlainSurface(UINT(size.x),
                                                        UINT(size.y),
                                                        D3DFMT_A32B32G32R32F,
                                                        D3DPOOL_SYSTEMMEM,
                                                        &select_readback_,
                                                        nullptr)))
    {
      d3d9_release(select_tx_);
      d3d9_release(select_readback_);
      return false;
    }
    select_width_ = size.x;
    select_height_ = size.y;
  }

  IDirect3DSurface9 *surface = nullptr;
  select_tx_->GetSurfaceLevel(0, &surface);
  D3DVIEWPORT9 viewport;
  device->GetViewport(&viewport);
  device->SetRenderTarget(0, surface);
  d3d9_release(surface);
  for (int i = 1; i < 4; i++) {
    device->SetRenderTarget(i, nullptr);
  }
  /* Clear all of it, the clear is limited to the viewport. */
  device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
  device->SetViewport(&viewport);

  /* The ids are written whatever the pass state is (depth only passes, blending). */
  device->GetRenderState(D3DRS_COLORWRITEENABLE, &select_saved_states_[0]);
  device->GetRenderState(D3DRS_ALPHABLENDENABLE, &select_saved_states_[1]);
  device->GetRenderState(D3DRS_SRGBWRITEENABLE, &select_saved_states_[2]);
  device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  return true;
}

void D3D9Context::select_draw_end()
{
  D3D9_HEAP_CHECK_SCOPE("D3D9Context::select_draw_end");
  D3D9FrameBuffer *fb = static_cast<D3D9FrameBuffer *>(active_fb);
  device->SetRenderState(D3DRS_COLORWRITEENABLE, select_saved_states_[0]);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, select_saved_states_[1]);
  device->SetRenderState(D3DRS_SRGBWRITEENABLE, select_saved_states_[2]);
  /* Bind the frame-buffer targets again on the next draw. */
  fb->tag_dirty();

  const D3D9UniformBuf *info_buf = bound_ubos[SELECT_DATA_SLOT];
  D3D9StorageBuf *out_buf = bound_ssbos[SELECT_ID_OUT_SLOT];
  if (info_buf == nullptr || out_buf == nullptr) {
    return;
  }
  /* SelectInfoData: int2 cursor, uint mode, int radius. */
  int32_t info[4];
  memcpy(info, info_buf->data_get(), sizeof(info));
  const int2 cursor(info[0], info[1]);
  const int mode = info[2];
  const int radius = info[3];

  IDirect3DSurface9 *surface = nullptr;
  select_tx_->GetSurfaceLevel(0, &surface);
  d3d9_frame_stats.readbacks++;
  const HRESULT hr = device->GetRenderTargetData(surface, select_readback_);
  d3d9_release(surface);
  D3DLOCKED_RECT locked;
  if (FAILED(hr) || FAILED(select_readback_->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
    return;
  }
  uint32_t *out = reinterpret_cast<uint32_t *>(out_buf->data_get_mutable());
  const size_t out_len = out_buf->size_get() / 4;
  const bool flipped = fb->is_flipped();
  for (int row = 0; row < select_height_; row++) {
    const float *pixel = reinterpret_cast<const float *>(static_cast<const uint8_t *>(locked.pBits) +
                                                         size_t(row) * locked.Pitch);
    /* Flipped targets have the OpenGL memory layout: the rows go up. */
    const int y = flipped ? row : select_height_ - 1 - row;
    for (int x = 0; x < select_width_; x++, pixel += 4) {
      if (pixel[0] < 0.5f) {
        continue;
      }
      const uint32_t id = uint32_t(pixel[0] - 0.5f);
      const float depth = clamp_f(pixel[1], 0.0f, 1.0f);
      if (mode == SELECT_ALL) {
        if (radius > 0) {
          const int2 coord = int2(x, y) - cursor;
          if (coord.x * coord.x + coord.y * coord.y > radius * radius) {
            continue;
          }
        }
        if (id / 32 < out_len) {
          out[id / 32] |= 1u << (id % 32);
        }
      }
      else if (mode == SELECT_PICK_ALL) {
        if (id < out_len) {
          out[id] = min_uu(out[id], float_as_uint(depth));
        }
      }
      else if (mode == SELECT_PICK_NEAREST) {
        const int dist = max_ii(abs(x - cursor.x), abs(y - cursor.y));
        if (dist < 0xFF && id < out_len) {
          const uint32_t packed = (uint32_t(depth * float(0x00FFFFFFu)) << 8u) | uint32_t(dist);
          out[id] = min_uu(out[id], packed);
        }
      }
    }
  }
  select_readback_->UnlockRect();
}

void D3D9Context::point_sprite_set(bool enable)
{
  if (point_sprite_ != int(enable)) {
    device->SetRenderState(D3DRS_POINTSPRITEENABLE, enable ? TRUE : FALSE);
    point_sprite_ = int(enable);
  }
}

void D3D9Context::flat_color_set(bool enable)
{
  if (flat_color_ != int(enable)) {
    device->SetRenderState(D3DRS_SHADEMODE, enable ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
    flat_color_ = int(enable);
  }
}

void D3D9Context::instance_id_set(int instance)
{
  reserved_constants_[0].w = float(instance);
  device->SetVertexShaderConstantF(0, &reserved_constants_[0].x, 1);
}

/** \} */

}  // namespace blender::gpu
