/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstdio>

#include "BLI_set.hh"
#include "BLI_string.h"

#include "gpu_capabilities_private.hh"
#include "gpu_platform_private.hh"
#include "gpu_texture_pool_private.hh"

#include "d3d9_backend.hh"
#include "d3d9_batch.hh"
#include "d3d9_buffers.hh"
#include "d3d9_context.hh"
#include "d3d9_framebuffer.hh"
#include "d3d9_index_buffer.hh"
#include "d3d9_shader.hh"
#include "d3d9_texture.hh"
#include "d3d9_vertex_buffer.hh"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.d3d9"};

void d3d9_heap_check(const char *where, int level)
{
  static const int enabled_level = getenv("BLENDER_D3D9_HEAPCHECK") ?
                                       atoi(getenv("BLENDER_D3D9_HEAPCHECK")) :
                                       0;
  static bool reported = false;
  static const char *last_ok = "start";
  if (level > enabled_level || reported) {
    return;
  }
  if (HeapValidate(GetProcessHeap(), 0, nullptr)) {
    last_ok = where;
    return;
  }
  reported = true;
  fprintf(stderr,
          "d3d9: HEAP CORRUPTED, found after \"%s\" (last valid after \"%s\")\n",
          where,
          last_ok);
  fflush(stderr);
}

bool d3d9_check(HRESULT hr, const char *call, const char *file, int line)
{
  if (SUCCEEDED(hr)) {
    return true;
  }
  /* Only report each failing call site once, failures tend to repeat every frame. */
  static Set<std::pair<const char *, int>> reported;
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  if (reported.add({file, line})) {
    CLOG_ERROR(&LOG, "%s failed (0x%08lx) at %s:%d", call, (unsigned long)hr, file, line);
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Platform
 * \{ */

D3D9Backend::D3D9Backend()
{
  d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
  platform_init();
  capabilities_init();
}

D3D9Backend::~D3D9Backend()
{
  d3d9_release(d3d_);
  GPG.clear();
}

bool D3D9Backend::is_supported()
{
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (d3d == nullptr) {
    return false;
  }
  D3DCAPS9 caps;
  const bool has_caps = SUCCEEDED(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps));
  d3d->Release();
  if (!has_caps) {
    return false;
  }
  /* Vertex shaders can be processed in software, pixel shaders cannot. */
  return caps.PixelShaderVersion >= D3DPS_VERSION(2, 0);
}

void D3D9Backend::platform_init()
{
  D3DADAPTER_IDENTIFIER9 id = {};
  if (d3d_) {
    d3d_->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
  }

  GPUDeviceType device = GPU_DEVICE_UNKNOWN;
  switch (id.VendorId) {
    case 0x1002:
      device = GPU_DEVICE_ATI;
      break;
    case 0x10DE:
      device = GPU_DEVICE_NVIDIA;
      break;
    case 0x8086:
      device = GPU_DEVICE_INTEL;
      break;
    default:
      break;
  }

  char vendor[32];
  switch (device) {
    case GPU_DEVICE_ATI:
      STRNCPY(vendor, "ATI Technologies Inc.");
      break;
    case GPU_DEVICE_NVIDIA:
      STRNCPY(vendor, "NVIDIA Corporation");
      break;
    case GPU_DEVICE_INTEL:
      STRNCPY(vendor, "Intel");
      break;
    default:
      SNPRINTF(vendor, "Vendor 0x%04lx", (unsigned long)id.VendorId);
      break;
  }

  char version[64];
  SNPRINTF(version,
           "Direct3D 9 (driver %u.%u.%u.%u)",
           uint(HIWORD(id.DriverVersion.HighPart)),
           uint(LOWORD(id.DriverVersion.HighPart)),
           uint(HIWORD(id.DriverVersion.LowPart)),
           uint(LOWORD(id.DriverVersion.LowPart)));

  GPG.init(device,
           GPU_OS_WIN,
           GPU_DRIVER_OFFICIAL,
           GPU_SUPPORT_LEVEL_SUPPORTED,
           GPU_BACKEND_D3D9,
           vendor,
           id.Description,
           version,
           GPU_ARCHITECTURE_IMR);
}

static const char *d3d9_extension_get(int /*index*/)
{
  return "";
}

void D3D9Backend::capabilities_init()
{
  if (d3d_) {
    d3d_->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    D3DDISPLAYMODE mode;
    if (SUCCEEDED(d3d_->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode))) {
      display_format = mode.Format;
    }
  }

  supports_separate_alpha_blend = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;
  supports_mrt_blending = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING) != 0;
  if (getenv("BLENDER_D3D9_FRAMETIME")) {
    auto check = [&](DWORD usage, D3DRESOURCETYPE type, D3DFORMAT format) {
      return d3d_ && SUCCEEDED(d3d_->CheckDeviceFormat(
                         D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, display_format, usage, type, format));
    };
    printf("d3d9: caps: srgb write back buffer %d, A8R8G8B8 texture %d; srgb read A8R8G8B8 %d; "
           "fp16 blend %d, A2R10G10B10 rt %d blend %d\n",
           int(check(D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_SURFACE, display_format)),
           int(check(D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)),
           int(check(D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)),
           int(check(D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F)),
           int(check(D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A2R10G10B10)),
           int(check(D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, D3DFMT_A2R10G10B10)));
  }
  supports_blend_op = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_BLENDOP) != 0;
  supports_non_pow2 = (caps.TextureCaps & D3DPTEXTURECAPS_POW2) == 0 ||
                      (caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0;
  supports_index_u32 = caps.MaxVertexIndex > 0xFFFF;
  supports_autogen_mipmap = (caps.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) != 0;
  supports_scissor = (caps.RasterCaps & D3DPRASTERCAPS_SCISSORTEST) != 0;

  GCaps.max_texture_size = int(min_uu(caps.MaxTextureWidth, caps.MaxTextureHeight));
  /* `BLENDER_D3D9_EMULATE_R300`: the texture size limit of the Radeon 9x00. */
  if (getenv("BLENDER_D3D9_EMULATE_R300")) {
    GCaps.max_texture_size = min_ii(GCaps.max_texture_size, 2048);
  }
  GCaps.max_texture_3d_size = int(caps.MaxVolumeExtent);
  GCaps.max_texture_layers = 1;
  GCaps.max_textures = 16;
  GCaps.max_images = 0;
  GCaps.max_uniforms_vert = int(caps.MaxVertexShaderConst) * 4;
  GCaps.max_uniforms_frag = 32 * 4;
  GCaps.max_batch_indices = int(min_uu(caps.MaxVertexIndex, 0x7FFFFFFF));
  GCaps.max_batch_vertices = int(min_uu(caps.MaxVertexIndex, 0x7FFFFFFF));
  GCaps.max_vertex_attribs = 16;
  GCaps.max_varying_floats = 8 * 4 + 2 * 4;
  GCaps.max_shader_storage_buffer_bindings = 0;
  GCaps.max_compute_shader_storage_blocks = 0;
  GCaps.max_uniform_buffer_size = 4096;
  GCaps.max_storage_buffer_size = size_t(64) * 1024 * 1024;
  GCaps.max_buffer_texture_size = 0;
  GCaps.extensions_len = 0;
  GCaps.extension_get = d3d9_extension_get;
  GCaps.mem_stats_support = true;
  GCaps.geometry_shader_support = false;
  GCaps.compute_shader_support = false;
  GCaps.float_blending_support = d3d_ && SUCCEEDED(d3d_->CheckDeviceFormat(
                                             D3DADAPTER_DEFAULT,
                                             D3DDEVTYPE_HAL,
                                             display_format,
                                             D3DUSAGE_RENDERTARGET |
                                                 D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                             D3DRTYPE_TEXTURE,
                                             D3DFMT_A16B16G16R16F));
  GCaps.hdr_viewport_support = false;
  GCaps.stencil_export_support = false;
  GCaps.max_parallel_compilations = 0;
  /* Compile shaders on the main thread: there is nothing to compile, only device objects to
   * create from precompiled bytecode. */
  GCaps.use_main_context_workaround = true;
  GCaps.texture_pool_workaround = false;
  GCaps.minimum_per_vertex_stride = 1;
}

bool D3D9Backend::format_supported(D3DFORMAT format, DWORD usage, D3DRESOURCETYPE type) const
{
  if (d3d_ == nullptr) {
    return false;
  }
  return SUCCEEDED(d3d_->CheckDeviceFormat(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, display_format, usage, type, format));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resources
 * \{ */

void D3D9Backend::init_resources()
{
  compiler_ = MEM_new<D3D9ShaderCompiler>(__func__);
}

IDirect3DTexture9 *D3D9Backend::texture_cache_take(UINT width,
                                                  UINT height,
                                                  UINT levels,
                                                  D3DFORMAT format)
{
  for (int i = 0; i < texture_cache_.size(); i++) {
    const CachedTexture &cached = texture_cache_[i];
    if (cached.width == width && cached.height == height && cached.levels == levels &&
        cached.format == format && cached.frame + 4 <= texture_cache_frame_)
    {
      IDirect3DTexture9 *texture = cached.texture;
      texture_cache_.remove_and_reorder(i);
      return texture;
    }
  }
  return nullptr;
}

void D3D9Backend::texture_cache_put(IDirect3DTexture9 *texture)
{
  D3DSURFACE_DESC desc;
  texture->GetLevelDesc(0, &desc);
  /* Only small textures (icons, text) are recycled: big ones of a size that keeps changing (the
   * frames of an external render engine) would pile up hundreds of MB. */
  if (desc.Width * desc.Height > 256 * 256) {
    texture->Release();
    return;
  }
  texture_cache_.append(
      {texture, desc.Width, desc.Height, texture->GetLevelCount(), desc.Format, texture_cache_frame_});
}

void D3D9Backend::texture_cache_frame()
{
  texture_cache_frame_++;
  /* Release the textures unused for a while, and the oldest ones beyond a count. */
  for (int i = texture_cache_.size() - 1; i >= 0; i--) {
    if (texture_cache_[i].frame + 600 < texture_cache_frame_ || texture_cache_.size() > 256) {
      texture_cache_[i].texture->Release();
      texture_cache_.remove_and_reorder(i);
    }
  }
}

IDirect3DVertexBuffer9 *D3D9Backend::point_corner_buffer()
{
  if (point_corner_buffer_ == nullptr) {
    /* Enough for the immediate mode buffer with the smallest vertex size. */
    const uint corners_len = D3D9_IMM_BUFFER_SIZE / 8;
    if (!D3D9_CHECK(device->CreateVertexBuffer(corners_len * sizeof(float[2]),
                                               D3DUSAGE_WRITEONLY,
                                               0,
                                               D3DPOOL_MANAGED,
                                               &point_corner_buffer_,
                                               nullptr)))
    {
      point_corner_buffer_ = nullptr;
      return nullptr;
    }
    float(*corners)[2];
    if (SUCCEEDED(point_corner_buffer_->Lock(0, 0, reinterpret_cast<void **>(&corners), 0))) {
      const float pattern[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
      for (uint i = 0; i < corners_len; i++) {
        corners[i][0] = pattern[i % 6][0];
        corners[i][1] = pattern[i % 6][1];
      }
      point_corner_buffer_->Unlock();
    }
  }
  return point_corner_buffer_;
}

void D3D9Backend::delete_resources()
{
  d3d9_release(point_corner_buffer_);
  for (CachedTexture &cached : texture_cache_) {
    cached.texture->Release();
  }
  texture_cache_.clear();
  MEM_delete(compiler_);
  compiler_ = nullptr;
  d3d9_release(vertex_id_buffer_);
  d3d9_release(line_loop_ibo_);
  d3d9_release(default_attr_buffer_);
  d3d9_release(dummy_color_);
  d3d9_release(blit_vs_);
  d3d9_release(blit_ps_);
  d3d9_release(blit_decl_);
}

IDirect3DVertexBuffer9 *D3D9Backend::vertex_id_buffer()
{
  if (vertex_id_buffer_ == nullptr) {
    if (!D3D9_CHECK(device->CreateVertexBuffer(D3D9_VERTEX_ID_LEN * sizeof(float),
                                               D3DUSAGE_WRITEONLY,
                                               0,
                                               D3DPOOL_MANAGED,
                                               &vertex_id_buffer_,
                                               nullptr)))
    {
      return nullptr;
    }
    float *data;
    vertex_id_buffer_->Lock(0, 0, reinterpret_cast<void **>(&data), 0);
    for (int i = 0; i < D3D9_VERTEX_ID_LEN; i++) {
      data[i] = float(i);
    }
    vertex_id_buffer_->Unlock();
  }
  return vertex_id_buffer_;
}

IDirect3DIndexBuffer9 *D3D9Backend::line_loop_index_buffer()
{
  if (line_loop_ibo_ == nullptr) {
    if (!D3D9_CHECK(device->CreateIndexBuffer(D3D9_VERTEX_ID_LEN * 2 * sizeof(uint16_t),
                                              D3DUSAGE_WRITEONLY,
                                              D3DFMT_INDEX16,
                                              D3DPOOL_MANAGED,
                                              &line_loop_ibo_,
                                              nullptr)))
    {
      return nullptr;
    }
    uint16_t *data;
    line_loop_ibo_->Lock(0, 0, reinterpret_cast<void **>(&data), 0);
    for (int i = 0; i < D3D9_VERTEX_ID_LEN; i++) {
      data[i * 2] = uint16_t(i);
      data[i * 2 + 1] = 0;
    }
    line_loop_ibo_->Unlock();
  }
  return line_loop_ibo_;
}

IDirect3DVertexBuffer9 *D3D9Backend::default_attr_buffer()
{
  if (default_attr_buffer_ == nullptr) {
    if (!D3D9_CHECK(device->CreateVertexBuffer(D3D9_VERTEX_ID_LEN * sizeof(D3DCOLOR),
                                               D3DUSAGE_WRITEONLY,
                                               0,
                                               D3DPOOL_MANAGED,
                                               &default_attr_buffer_,
                                               nullptr)))
    {
      return nullptr;
    }
    void *data;
    default_attr_buffer_->Lock(0, 0, &data, 0);
    memset(data, 0xFF, D3D9_VERTEX_ID_LEN * sizeof(D3DCOLOR));
    default_attr_buffer_->Unlock();
  }
  return default_attr_buffer_;
}

IDirect3DSurface9 *D3D9Backend::dummy_color_target(int width, int height)
{
  if (dummy_color_ == nullptr || dummy_color_size_[0] < width || dummy_color_size_[1] < height) {
    d3d9_release(dummy_color_);
    dummy_color_size_[0] = max_ii(width, dummy_color_size_[0]);
    dummy_color_size_[1] = max_ii(height, dummy_color_size_[1]);
    if (!D3D9_CHECK(device->CreateRenderTarget(dummy_color_size_[0],
                                               dummy_color_size_[1],
                                               D3DFMT_A8R8G8B8,
                                               D3DMULTISAMPLE_NONE,
                                               0,
                                               FALSE,
                                               &dummy_color_,
                                               nullptr)))
    {
      dummy_color_size_[0] = dummy_color_size_[1] = 0;
      return nullptr;
    }
  }
  return dummy_color_;
}

void D3D9Backend::draw_texture_rect(IDirect3DBaseTexture9 *texture,
                                    const RECT &dst_rect,
                                    const bool flip_v)
{
  if (blit_vs_ == nullptr) {
    const D3D9ShaderBinary *binary = d3d9_shader_binary_find("d3d9_internal_blit");
    if (binary == nullptr ||
        !D3D9_CHECK(device->CreateVertexShader(reinterpret_cast<const DWORD *>(binary->vs),
                                               &blit_vs_)) ||
        !D3D9_CHECK(device->CreatePixelShader(reinterpret_cast<const DWORD *>(binary->ps),
                                              &blit_ps_)))
    {
      return;
    }
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END(),
    };
    D3D9_CHECK(device->CreateVertexDeclaration(elements, &blit_decl_));
  }
  const int width = dst_rect.right - dst_rect.left;
  const int height = dst_rect.bottom - dst_rect.top;
  if (width <= 0 || height <= 0 || texture == nullptr) {
    return;
  }

  D3DVIEWPORT9 vp = {DWORD(dst_rect.left), DWORD(dst_rect.top), DWORD(width), DWORD(height), 0.0f, 1.0f};
  device->SetViewport(&vp);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  device->SetVertexShader(blit_vs_);
  device->SetPixelShader(blit_ps_);
  device->SetVertexDeclaration(blit_decl_);
  device->SetTexture(0, texture);
  device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

  /* Clip space corners, shifted by half a pixel (D3D9 pixel centers). */
  const float dx = -1.0f / float(width), dy = 1.0f / float(height);
  const float v_top = flip_v ? 1.0f : 0.0f, v_bottom = 1.0f - v_top;
  const float quad[4][4] = {
      {-1.0f + dx, 1.0f + dy, 0.0f, v_top},
      {1.0f + dx, 1.0f + dy, 1.0f, v_top},
      {-1.0f + dx, -1.0f + dy, 0.0f, v_bottom},
      {1.0f + dx, -1.0f + dy, 1.0f, v_bottom},
  };
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Allocators
 * \{ */

Context *D3D9Backend::context_alloc(GHOST_IWindow *ghost_window, GHOST_IContext *ghost_context)
{
  return new D3D9Context(ghost_window, ghost_context);
}

Batch *D3D9Backend::batch_alloc()
{
  return new D3D9Batch();
}

Fence *D3D9Backend::fence_alloc()
{
  return new D3D9Fence();
}

FrameBuffer *D3D9Backend::framebuffer_alloc(const char *name)
{
  return new D3D9FrameBuffer(name);
}

IndexBuf *D3D9Backend::indexbuf_alloc()
{
  return new D3D9IndexBuf();
}

PixelBuffer *D3D9Backend::pixelbuf_alloc(size_t size)
{
  return new D3D9PixelBuffer(size);
}

QueryPool *D3D9Backend::querypool_alloc()
{
  return new D3D9QueryPool();
}

Shader *D3D9Backend::shader_alloc(const char *name)
{
  return new D3D9Shader(name);
}

Texture *D3D9Backend::texture_alloc(const char *name)
{
  return new D3D9Texture(name);
}

TexturePool *D3D9Backend::texturepool_alloc()
{
  return new TexturePoolImpl();
}

UniformBuf *D3D9Backend::uniformbuf_alloc(size_t size, const char *name)
{
  return new D3D9UniformBuf(size, name);
}

StorageBuf *D3D9Backend::storagebuf_alloc(size_t size, GPUUsageType /*usage*/, const char *name)
{
  return new D3D9StorageBuf(size, name);
}

VertBuf *D3D9Backend::vertbuf_alloc()
{
  return new D3D9VertBuf();
}

void D3D9Backend::compute_dispatch(int /*groups_x_len*/,
                                   int /*groups_y_len*/,
                                   int /*groups_z_len*/)
{
  /* No compute on SM2.0. */
}

void D3D9Backend::compute_dispatch_indirect(StorageBuf * /*indirect_buf*/) {}

/** \} */

}  // namespace blender::gpu
