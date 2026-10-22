/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Textures are created on first use: as `D3DPOOL_MANAGED` textures when data is uploaded first
 * (they survive device loss and can be locked), as `D3DPOOL_DEFAULT` render targets when they are
 * first attached to a frame-buffer. A managed texture attached later is converted.
 */

#include <cstring>

#include "BLI_math_base.h"
#include "BLI_math_half.hh"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "gpu_capabilities_private.hh"

#include "d3d9_backend.hh"
#include "d3d9_context.hh"
#include "d3d9_state.hh"
#include "d3d9_texture.hh"
#include "d3d9_vertex_buffer.hh"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.d3d9"};

/* -------------------------------------------------------------------- */
/** \name Formats
 * \{ */

D3D9TextureFormat d3d9_texture_format(TextureFormat format)
{
  using K = D3D9TexelKind;
  switch (format) {
    case TextureFormat::UNORM_8:
      return {D3DFMT_L8, K::UNORM8, 1};
    case TextureFormat::SFLOAT_16_16_16_16:
    case TextureFormat::SFLOAT_16_16_16:
    case TextureFormat::UFLOAT_11_11_10:
    case TextureFormat::UNORM_16_16_16_16:
    case TextureFormat::UNORM_16_16_16:
    case TextureFormat::SNORM_16_16_16_16:
    case TextureFormat::SNORM_16_16_16:
      return {D3DFMT_A16B16G16R16F, K::HALF, 4};
    case TextureFormat::SFLOAT_16_16:
    case TextureFormat::UNORM_16_16:
    case TextureFormat::SNORM_16_16:
      return {D3DFMT_G16R16F, K::HALF, 2};
    case TextureFormat::SFLOAT_16:
    case TextureFormat::UNORM_16:
    case TextureFormat::SNORM_16:
      return {D3DFMT_R16F, K::HALF, 1};
    case TextureFormat::SFLOAT_32_32_32_32:
    case TextureFormat::SFLOAT_32_32_32:
    case TextureFormat::UINT_32_32_32_32:
    case TextureFormat::SINT_32_32_32_32:
    case TextureFormat::UINT_16_16_16_16:
    case TextureFormat::SINT_16_16_16_16:
    case TextureFormat::UINT_8_8_8_8:
    case TextureFormat::SINT_8_8_8_8:
    case TextureFormat::UINT_32_32_32:
    case TextureFormat::SINT_32_32_32:
    case TextureFormat::UINT_16_16_16:
    case TextureFormat::SINT_16_16_16:
    case TextureFormat::UINT_8_8_8:
    case TextureFormat::SINT_8_8_8:
      return {D3DFMT_A32B32G32R32F, K::FLOAT, 4};
    case TextureFormat::SFLOAT_32_32:
    case TextureFormat::UINT_32_32:
    case TextureFormat::SINT_32_32:
    case TextureFormat::UINT_16_16:
    case TextureFormat::SINT_16_16:
    case TextureFormat::UINT_8_8:
    case TextureFormat::SINT_8_8:
      return {D3DFMT_G32R32F, K::FLOAT, 2};
    case TextureFormat::SFLOAT_32:
    case TextureFormat::UINT_32:
    case TextureFormat::SINT_32:
    case TextureFormat::UINT_16:
    case TextureFormat::SINT_16:
    case TextureFormat::UINT_8:
    case TextureFormat::SINT_8:
      return {D3DFMT_R32F, K::FLOAT, 1};
    case TextureFormat::SRGB_DXT1:
    case TextureFormat::SNORM_DXT1:
      return {D3DFMT_DXT1, K::COMPRESSED, 4};
    case TextureFormat::SRGB_DXT3:
    case TextureFormat::SNORM_DXT3:
      return {D3DFMT_DXT3, K::COMPRESSED, 4};
    case TextureFormat::SRGB_DXT5:
    case TextureFormat::SNORM_DXT5:
      return {D3DFMT_DXT5, K::COMPRESSED, 4};
    case TextureFormat::SFLOAT_32_DEPTH_UINT_8:
      return {D3DFMT_D24S8, K::DEPTH, 1};
    case TextureFormat::SFLOAT_32_DEPTH:
      return {D3DFMT_D24X8, K::DEPTH, 1};
    case TextureFormat::UNORM_16_DEPTH:
      return {D3DFMT_D16, K::DEPTH, 1};
    default:
      /* All remaining 8 bit (and 10 bit) normalized formats. */
      return {D3DFMT_A8R8G8B8, K::UNORM8, 4};
  }
}

static int d3d9_texel_size(const D3D9TextureFormat &format)
{
  switch (format.kind) {
    case D3D9TexelKind::UNORM8:
      return format.components == 1 ? 1 : 4;
    case D3D9TexelKind::HALF:
      return format.components * 2;
    case D3D9TexelKind::FLOAT:
      return format.components * 4;
    default:
      return 4;
  }
}

static int dxt_block_size(D3DFORMAT format)
{
  return format == D3DFMT_DXT1 ? 8 : 16;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texel conversion
 * \{ */

/** Read one host texel as 4 floats. Integer data is normalized for normalized formats. */
static void texel_decode(const uint8_t *src,
                         eGPUDataFormat data_format,
                         int components,
                         bool normalized,
                         float r_value[4])
{
  r_value[0] = r_value[1] = r_value[2] = 0.0f;
  r_value[3] = 1.0f;
  switch (data_format) {
    case GPU_DATA_FLOAT: {
      const float *f = reinterpret_cast<const float *>(src);
      for (int c = 0; c < components; c++) {
        r_value[c] = f[c];
      }
      break;
    }
    case GPU_DATA_HALF_FLOAT: {
      const uint16_t *h = reinterpret_cast<const uint16_t *>(src);
      for (int c = 0; c < components; c++) {
        r_value[c] = math::half_to_float(h[c]);
      }
      break;
    }
    case GPU_DATA_UBYTE: {
      for (int c = 0; c < components; c++) {
        r_value[c] = normalized ? float(src[c]) / 255.0f : float(src[c]);
      }
      break;
    }
    case GPU_DATA_INT: {
      const int32_t *v = reinterpret_cast<const int32_t *>(src);
      for (int c = 0; c < components; c++) {
        r_value[c] = float(v[c]);
      }
      break;
    }
    case GPU_DATA_UINT: {
      const uint32_t *v = reinterpret_cast<const uint32_t *>(src);
      for (int c = 0; c < components; c++) {
        r_value[c] = normalized ? float(v[c]) / 4294967295.0f : float(v[c]);
      }
      break;
    }
    case GPU_DATA_2_10_10_10_REV: {
      uint32_t v;
      memcpy(&v, src, 4);
      r_value[0] = float(v & 0x3FF) / 1023.0f;
      r_value[1] = float((v >> 10) & 0x3FF) / 1023.0f;
      r_value[2] = float((v >> 20) & 0x3FF) / 1023.0f;
      r_value[3] = float(v >> 30) / 3.0f;
      break;
    }
    case GPU_DATA_10_11_11_REV:
    case GPU_DATA_UINT_24_8_DEPRECATED:
    default:
      break;
  }
}

/** Write 4 floats as one host texel. */
static void texel_encode(const float value[4],
                         eGPUDataFormat data_format,
                         int components,
                         bool normalized,
                         uint8_t *dst)
{
  switch (data_format) {
    case GPU_DATA_FLOAT: {
      float *f = reinterpret_cast<float *>(dst);
      for (int c = 0; c < components; c++) {
        f[c] = value[c];
      }
      break;
    }
    case GPU_DATA_HALF_FLOAT: {
      uint16_t *h = reinterpret_cast<uint16_t *>(dst);
      for (int c = 0; c < components; c++) {
        h[c] = math::float_to_half(value[c]);
      }
      break;
    }
    case GPU_DATA_UBYTE: {
      for (int c = 0; c < components; c++) {
        const float v = normalized ? value[c] * 255.0f + 0.5f : value[c];
        dst[c] = uint8_t(clamp_f(v, 0.0f, 255.0f));
      }
      break;
    }
    case GPU_DATA_INT: {
      int32_t *v = reinterpret_cast<int32_t *>(dst);
      for (int c = 0; c < components; c++) {
        v[c] = int32_t(value[c]);
      }
      break;
    }
    case GPU_DATA_UINT: {
      uint32_t *v = reinterpret_cast<uint32_t *>(dst);
      for (int c = 0; c < components; c++) {
        v[c] = uint32_t(max_ff(value[c], 0.0f));
      }
      break;
    }
    default:
      memset(dst, 0, 4);
      break;
  }
}

/** Read one device texel as 4 floats. */
static void device_texel_decode(const D3D9TextureFormat &format,
                                const uint8_t *src,
                                float r_value[4])
{
  r_value[0] = r_value[1] = r_value[2] = 0.0f;
  r_value[3] = 1.0f;
  switch (format.kind) {
    case D3D9TexelKind::UNORM8:
      if (format.components == 1) {
        r_value[0] = float(src[0]) / 255.0f;
      }
      else {
        /* BGRA in memory. */
        r_value[0] = float(src[2]) / 255.0f;
        r_value[1] = float(src[1]) / 255.0f;
        r_value[2] = float(src[0]) / 255.0f;
        r_value[3] = float(src[3]) / 255.0f;
      }
      break;
    case D3D9TexelKind::HALF: {
      const uint16_t *h = reinterpret_cast<const uint16_t *>(src);
      for (int c = 0; c < format.components; c++) {
        r_value[c] = math::half_to_float(h[c]);
      }
      break;
    }
    case D3D9TexelKind::FLOAT: {
      const float *f = reinterpret_cast<const float *>(src);
      for (int c = 0; c < format.components; c++) {
        r_value[c] = f[c];
      }
      break;
    }
    default:
      break;
  }
}

/** Write 4 floats as one device texel. */
static void device_texel_encode(const D3D9TextureFormat &format,
                                const float value[4],
                                uint8_t *dst)
{
  switch (format.kind) {
    case D3D9TexelKind::UNORM8: {
      auto to_byte = [](float v) { return uint8_t(clamp_f(v * 255.0f + 0.5f, 0.0f, 255.0f)); };
      if (format.components == 1) {
        dst[0] = to_byte(value[0]);
      }
      else {
        dst[0] = to_byte(value[2]);
        dst[1] = to_byte(value[1]);
        dst[2] = to_byte(value[0]);
        dst[3] = to_byte(value[3]);
      }
      break;
    }
    case D3D9TexelKind::HALF: {
      uint16_t *h = reinterpret_cast<uint16_t *>(dst);
      for (int c = 0; c < format.components; c++) {
        h[c] = math::float_to_half(value[c]);
      }
      break;
    }
    case D3D9TexelKind::FLOAT: {
      float *f = reinterpret_cast<float *>(dst);
      for (int c = 0; c < format.components; c++) {
        f[c] = value[c];
      }
      break;
    }
    default:
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Creation
 * \{ */

D3D9Texture::D3D9Texture(const char *name) : Texture(name) {}

D3D9Texture::~D3D9Texture()
{
  D3D9Context *ctx = D3D9Context::get();
  if (ctx) {
    ctx->state_manager->texture_unbind(this);
  }
  free_resource();
}

void D3D9Texture::free_resource()
{
  d3d9_release(tex2d_);
  d3d9_release(cube_);
  d3d9_release(volume_);
  d3d9_release(depth_surface_);
  is_render_target_ = false;
}

bool D3D9Texture::init_internal()
{
  device_format_ = d3d9_texture_format(format_);
  D3D9Backend *backend = D3D9Backend::get();

  device_w_ = max_ii(w_, 1);
  device_h_ = (type_ & GPU_TEXTURE_1D) ? 1 : max_ii(h_, 1);
  device_d_ = (type_ == GPU_TEXTURE_3D) ? max_ii(d_, 1) : 1;
  if (type_ == GPU_TEXTURE_1D_ARRAY) {
    /* Layers become rows. */
    device_h_ = max_ii(h_, 1);
  }

  const int max_size = GCaps.max_texture_size;
  if (device_w_ > max_size || device_h_ > max_size) {
    CLOG_WARN(&LOG,
              "Texture \"%s\" is too big (%dx%d, maximum %d)",
              name_.c_str(),
              device_w_,
              device_h_,
              max_size);
    return false;
  }

  const bool is_pow2 = is_power_of_2_i(device_w_) && is_power_of_2_i(device_h_) &&
                       is_power_of_2_i(device_d_);
  if (!is_pow2 && !backend->supports_non_pow2) {
    CLOG_WARN(&LOG, "Texture \"%s\" size is not a power of two", name_.c_str());
  }
  /* Conditional non power of two textures can't have mip-maps. */
  device_levels_ = is_pow2 ? max_ii(mipmaps_, 1) : 1;
  if (device_format_.kind == D3D9TexelKind::DEPTH) {
    device_levels_ = 1;
  }
  return true;
}

bool D3D9Texture::init_internal(VertBuf *vbo)
{
  /* Buffer textures become a single row texture. */
  type_ = GPU_TEXTURE_2D;
  h_ = 1;
  w_ = max_ii(min_ii(w_, GCaps.max_texture_size), 1);
  mipmaps_ = 1;
  if (!init_internal()) {
    return false;
  }
  D3D9VertBuf *d3d9_vbo = static_cast<D3D9VertBuf *>(vbo);
  const uchar *data = d3d9_vbo->host_data_get();
  if (data && device_format_.kind != D3D9TexelKind::DEPTH) {
    int offset[3] = {0, 0, 0};
    int extent[3] = {w_, 1, 1};
    const bool is_float = (format_flag_ & GPU_FORMAT_FLOAT) != 0;
    const bool is_int = (format_flag_ & GPU_FORMAT_INTEGER) != 0;
    const eGPUDataFormat data_format = is_float ? GPU_DATA_FLOAT :
                                       is_int   ? ((format_flag_ & GPU_FORMAT_SIGNED) ?
                                                       GPU_DATA_INT :
                                                       GPU_DATA_UINT) :
                                                  GPU_DATA_UBYTE;
    update_sub(0, offset, extent, data_format, data, 0);
  }
  return true;
}

bool D3D9Texture::init_internal(gpu::Texture *src,
                                int mip_offset,
                                int /*layer_offset*/,
                                bool /*use_stencil*/)
{
  source_ = static_cast<D3D9Texture *>(src);
  view_mip_offset_ = mip_offset;
  device_format_ = source_->device_format_;
  device_w_ = source_->device_w_;
  device_h_ = source_->device_h_;
  device_d_ = source_->device_d_;
  device_levels_ = source_->device_levels_;
  return true;
}

bool D3D9Texture::ensure_resource(bool render_target)
{
  if (source_) {
    return source_->ensure_resource(render_target);
  }
  D3D9Backend *backend = D3D9Backend::get();
  IDirect3DDevice9 *device = backend->device;

  if (device_format_.kind == D3D9TexelKind::DEPTH) {
    if (depth_surface_ == nullptr) {
      D3D9_CHECK(device->CreateDepthStencilSurface(device_w_,
                                                   device_h_,
                                                   device_format_.format,
                                                   D3DMULTISAMPLE_NONE,
                                                   0,
                                                   FALSE,
                                                   &depth_surface_,
                                                   nullptr));
      is_render_target_ = true;
    }
    return depth_surface_ != nullptr;
  }

  const bool has_resource = tex2d_ || cube_ || volume_;
  if (has_resource) {
    if (render_target && !is_render_target_) {
      return convert_to_render_target();
    }
    return true;
  }

  filterable_ = ELEM(device_format_.kind, D3D9TexelKind::UNORM8, D3D9TexelKind::COMPRESSED) ||
                backend->format_supported(
                    device_format_.format, D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE);

  const D3DPOOL pool = render_target ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
  DWORD usage = render_target ? D3DUSAGE_RENDERTARGET : 0;
  HRESULT hr = E_FAIL;
  if (type_ & GPU_TEXTURE_CUBE) {
    hr = device->CreateCubeTexture(
        device_w_, device_levels_, usage, device_format_.format, pool, &cube_, nullptr);
  }
  else if (type_ == GPU_TEXTURE_3D && !render_target) {
    hr = device->CreateVolumeTexture(device_w_,
                                     device_h_,
                                     device_d_,
                                     device_levels_,
                                     0,
                                     device_format_.format,
                                     D3DPOOL_MANAGED,
                                     &volume_,
                                     nullptr);
  }
  else {
    hr = device->CreateTexture(device_w_,
                               device_h_,
                               device_levels_,
                               usage,
                               device_format_.format,
                               pool,
                               &tex2d_,
                               nullptr);
  }
  if (FAILED(hr)) {
    CLOG_ERROR(&LOG,
               "Creating texture \"%s\" (%dx%d, format %d, %s) failed (0x%08lx)",
               name_.c_str(),
               device_w_,
               device_h_,
               int(device_format_.format),
               render_target ? "render target" : "managed",
               (unsigned long)hr);
    return false;
  }
  is_render_target_ = render_target;
  return true;
}

bool D3D9Texture::convert_to_render_target()
{
  /* Only 2D textures are rendered to. */
  if (tex2d_ == nullptr) {
    return false;
  }
  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  IDirect3DTexture9 *managed = tex2d_;
  tex2d_ = nullptr;
  if (!ensure_resource(true)) {
    tex2d_ = managed;
    return false;
  }
  /* Copy the existing content: managed level -> system memory surface -> render target. */
  for (int level = 0; level < device_levels_; level++) {
    D3DSURFACE_DESC desc;
    managed->GetLevelDesc(level, &desc);
    IDirect3DSurface9 *staging = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(
            desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr)))
    {
      continue;
    }
    D3DLOCKED_RECT src, dst;
    if (SUCCEEDED(managed->LockRect(level, &src, nullptr, D3DLOCK_READONLY))) {
      if (SUCCEEDED(staging->LockRect(&dst, nullptr, 0))) {
        const int row_size = int(desc.Width) * d3d9_texel_size(device_format_);
        for (uint y = 0; y < desc.Height; y++) {
          memcpy(static_cast<uint8_t *>(dst.pBits) + y * dst.Pitch,
                 static_cast<const uint8_t *>(src.pBits) + y * src.Pitch,
                 row_size);
        }
        staging->UnlockRect();
      }
      managed->UnlockRect(level);
    }
    IDirect3DSurface9 *target = level_surface(level, 0);
    if (target) {
      device->UpdateSurface(staging, nullptr, target, nullptr);
      target->Release();
    }
    staging->Release();
  }
  managed->Release();
  return true;
}

IDirect3DBaseTexture9 *D3D9Texture::base_texture_get()
{
  if (source_) {
    return source_->base_texture_get();
  }
  if (device_format_.kind == D3D9TexelKind::DEPTH) {
    /* SM2.0 hardware can't sample depth. */
    return nullptr;
  }
  if (!ensure_resource(false)) {
    return nullptr;
  }
  if (tex2d_) {
    return tex2d_;
  }
  if (cube_) {
    return cube_;
  }
  return volume_;
}

IDirect3DSurface9 *D3D9Texture::level_surface(int mip, int layer)
{
  IDirect3DSurface9 *surface = nullptr;
  if (tex2d_) {
    tex2d_->GetSurfaceLevel(mip, &surface);
  }
  else if (cube_) {
    cube_->GetCubeMapSurface(D3DCUBEMAP_FACES(clamp_i(layer, 0, 5)), mip, &surface);
  }
  return surface;
}

IDirect3DSurface9 *D3D9Texture::surface_get(int mip, int layer)
{
  if (source_) {
    return source_->surface_get(mip + view_mip_offset_, layer);
  }
  if (!ensure_resource(true)) {
    return nullptr;
  }
  if (depth_surface_) {
    depth_surface_->AddRef();
    return depth_surface_;
  }
  return level_surface(clamp_i(mip, 0, device_levels_ - 1), max_ii(layer, 0));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data upload
 * \{ */

void D3D9Texture::convert_upload(const void *src_data,
                                 eGPUDataFormat src_format,
                                 int width,
                                 int height,
                                 int src_row_len,
                                 uint8_t *dst,
                                 int dst_pitch) const
{
  const int src_components = to_component_len(format_);
  const size_t src_texel = to_bytesize(format_, src_format);
  const size_t src_pitch = size_t(src_row_len) * src_texel;
  const int dst_texel = d3d9_texel_size(device_format_);
  const bool normalized = (format_flag_ & GPU_FORMAT_NORMALIZED_INTEGER) != 0;
  const uint8_t *src = static_cast<const uint8_t *>(src_data);

  for (int y = 0; y < height; y++) {
    const uint8_t *s = src + y * src_pitch;
    uint8_t *d = dst + y * dst_pitch;

    /* Fast paths for the common cases. */
    if (device_format_.kind == D3D9TexelKind::UNORM8 && src_format == GPU_DATA_UBYTE) {
      if (device_format_.components == 1 && src_components == 1) {
        memcpy(d, s, width);
        continue;
      }
      if (src_components == 4) {
        for (int x = 0; x < width; x++, s += 4, d += 4) {
          d[0] = s[2];
          d[1] = s[1];
          d[2] = s[0];
          d[3] = s[3];
        }
        continue;
      }
    }
    if (src_components == device_format_.components &&
        ((device_format_.kind == D3D9TexelKind::HALF && src_format == GPU_DATA_HALF_FLOAT) ||
         (device_format_.kind == D3D9TexelKind::FLOAT && src_format == GPU_DATA_FLOAT)))
    {
      memcpy(d, s, size_t(width) * dst_texel);
      continue;
    }

    float texel[4];
    for (int x = 0; x < width; x++, s += src_texel, d += dst_texel) {
      texel_decode(s, src_format, src_components, normalized, texel);
      device_texel_encode(device_format_, texel, d);
    }
  }
}

void D3D9Texture::update_sub(int mip,
                             int offset[3],
                             int extent[3],
                             eGPUDataFormat format,
                             const void *data,
                             uint unpack_row_length)
{
  if (source_ || data == nullptr || device_format_.kind == D3D9TexelKind::DEPTH) {
    return;
  }
  if (mip >= device_levels_ || !ensure_resource(false)) {
    return;
  }

  int x = offset[0], y = 0, layer = 0;
  int width = extent[0], height = 1, depth = 1;
  switch (type_) {
    case GPU_TEXTURE_1D:
      break;
    case GPU_TEXTURE_1D_ARRAY:
    case GPU_TEXTURE_2D:
      y = offset[1];
      height = extent[1];
      break;
    case GPU_TEXTURE_2D_ARRAY:
    case GPU_TEXTURE_CUBE:
    case GPU_TEXTURE_CUBE_ARRAY:
      y = offset[1];
      height = extent[1];
      layer = offset[2];
      depth = extent[2];
      break;
    case GPU_TEXTURE_3D:
      y = offset[1];
      height = extent[1];
      depth = extent[2];
      break;
    default:
      height = max_ii(extent[1], 1);
      break;
  }
  const int src_row_len = unpack_row_length ? int(unpack_row_length) : width;
  const size_t src_texel = to_bytesize(format_, format);
  const size_t src_layer_size = size_t(src_row_len) * height * src_texel;
  const uint8_t *src = static_cast<const uint8_t *>(data);

  IDirect3DDevice9 *device = D3D9Backend::get()->device;

  if (device_format_.kind == D3D9TexelKind::COMPRESSED) {
    /* Data is already in blocks. */
    if (tex2d_ == nullptr || is_render_target_) {
      return;
    }
    RECT rect = {x, y, x + width, y + height};
    D3DLOCKED_RECT locked;
    if (D3D9_CHECK(tex2d_->LockRect(mip, &locked, &rect, 0))) {
      const int block_rows = (height + 3) / 4;
      const int row_size = ((width + 3) / 4) * dxt_block_size(device_format_.format);
      for (int row = 0; row < block_rows; row++) {
        memcpy(static_cast<uint8_t *>(locked.pBits) + row * locked.Pitch,
               src + row * row_size,
               row_size);
      }
      tex2d_->UnlockRect(mip);
    }
    return;
  }

  if (volume_) {
    D3DBOX box = {UINT(x), UINT(y), UINT(x + width), UINT(y + height), UINT(offset[2]),
                  UINT(offset[2] + depth)};
    D3DLOCKED_BOX locked;
    if (D3D9_CHECK(volume_->LockBox(mip, &locked, &box, 0))) {
      for (int z = 0; z < depth; z++) {
        convert_upload(src + z * src_layer_size,
                       format,
                       width,
                       height,
                       src_row_len,
                       static_cast<uint8_t *>(locked.pBits) + z * locked.SlicePitch,
                       locked.RowPitch);
      }
      volume_->UnlockBox(mip);
    }
    return;
  }

  /* 2D textures and cube faces. Array textures only keep their first layer. */
  for (int l = 0; l < depth; l++) {
    const int face = layer + l;
    if (tex2d_ && face != 0) {
      break;
    }
    if (cube_ && face > 5) {
      break;
    }
    const uint8_t *layer_src = src + l * src_layer_size;
    RECT rect = {x, y, x + width, y + height};

    if (!is_render_target_) {
      D3DLOCKED_RECT locked;
      HRESULT hr = tex2d_ ? tex2d_->LockRect(mip, &locked, &rect, 0) :
                            cube_->LockRect(D3DCUBEMAP_FACES(face), mip, &locked, &rect, 0);
      if (D3D9_CHECK(hr)) {
        convert_upload(layer_src,
                       format,
                       width,
                       height,
                       src_row_len,
                       static_cast<uint8_t *>(locked.pBits),
                       locked.Pitch);
        if (tex2d_) {
          tex2d_->UnlockRect(mip);
        }
        else {
          cube_->UnlockRect(D3DCUBEMAP_FACES(face), mip);
        }
      }
      continue;
    }

    /* Render targets can't be locked: go through a system memory surface. */
    IDirect3DSurface9 *staging = nullptr;
    if (!D3D9_CHECK(device->CreateOffscreenPlainSurface(
            width, height, device_format_.format, D3DPOOL_SYSTEMMEM, &staging, nullptr)))
    {
      return;
    }
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(staging->LockRect(&locked, nullptr, 0))) {
      convert_upload(layer_src,
                     format,
                     width,
                     height,
                     src_row_len,
                     static_cast<uint8_t *>(locked.pBits),
                     locked.Pitch);
      staging->UnlockRect();
      IDirect3DSurface9 *target = level_surface(mip, face);
      if (target) {
        POINT dst_point = {x, y};
        D3D9_CHECK(device->UpdateSurface(staging, nullptr, target, &dst_point));
        target->Release();
      }
    }
    staging->Release();
  }
}

void D3D9Texture::update_sub(int offset[3],
                             int extent[3],
                             eGPUDataFormat format,
                             GPUPixelBuffer *pixbuf)
{
  PixelBuffer *buffer = unwrap(pixbuf);
  update_sub(0, offset, extent, format, buffer->map(), 0);
}

void D3D9Texture::clear(double4 data)
{
  if (source_) {
    source_->clear(data);
    return;
  }
  if (device_format_.kind == D3D9TexelKind::DEPTH || !ensure_resource(false)) {
    return;
  }
  /* Encode one texel in the host float format and upload it to every level. */
  const int components = to_component_len(format_);
  const float value[4] = {float(data[0]), float(data[1]), float(data[2]), float(data[3])};
  for (int level = 0; level < device_levels_; level++) {
    int size[3] = {1, 1, 1};
    mip_size_get(level, size);
    const int width = max_ii(size[0], 1);
    const int height = (type_ & GPU_TEXTURE_1D) && type_ != GPU_TEXTURE_1D_ARRAY ?
                           1 :
                           max_ii(size[1], 1);
    const int layers = (type_ & GPU_TEXTURE_CUBE) ? 6 : 1;
    Vector<float> texels(size_t(width) * height * layers * components);
    for (int i = 0; i < width * height * layers; i++) {
      for (int c = 0; c < components; c++) {
        texels[i * components + c] = value[c];
      }
    }
    int offset[3] = {0, 0, 0};
    int extent[3] = {width, height, layers};
    update_sub(level, offset, extent, GPU_DATA_FLOAT, texels.data(), 0);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Read-back
 * \{ */

void D3D9Texture::read(int mip, eGPUDataFormat format, void *r_data)
{
  const size_t size = read_size_get(mip, format);
  memset(r_data, 0, size);
  if (source_) {
    source_->read(mip + view_mip_offset_, format, r_data);
    return;
  }
  if (device_format_.kind == D3D9TexelKind::DEPTH ||
      device_format_.kind == D3D9TexelKind::COMPRESSED || !ensure_resource(false) ||
      tex2d_ == nullptr || mip >= device_levels_)
  {
    return;
  }

  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  D3DSURFACE_DESC desc;
  tex2d_->GetLevelDesc(mip, &desc);

  IDirect3DSurface9 *readable = nullptr;
  bool from_staging = false;
  if (is_render_target_) {
    D3D9Context *ctx = D3D9Context::get();
    if (ctx) {
      ctx->end_scene();
    }
    if (!D3D9_CHECK(device->CreateOffscreenPlainSurface(
            desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &readable, nullptr)))
    {
      return;
    }
    IDirect3DSurface9 *target = level_surface(mip, 0);
    const bool ok = target && D3D9_CHECK(device->GetRenderTargetData(target, readable));
    if (target) {
      target->Release();
    }
    if (!ok) {
      readable->Release();
      return;
    }
    from_staging = true;
  }

  D3DLOCKED_RECT locked;
  HRESULT hr = from_staging ? readable->LockRect(&locked, nullptr, D3DLOCK_READONLY) :
                              tex2d_->LockRect(mip, &locked, nullptr, D3DLOCK_READONLY);
  if (SUCCEEDED(hr)) {
    const int components = to_component_len(format_);
    const size_t dst_texel = to_bytesize(format_, format);
    const int src_texel = d3d9_texel_size(device_format_);
    const bool normalized = (format_flag_ & GPU_FORMAT_NORMALIZED_INTEGER) != 0;
    uint8_t *dst = static_cast<uint8_t *>(r_data);
    float texel[4];
    for (uint y = 0; y < desc.Height; y++) {
      const uint8_t *s = static_cast<const uint8_t *>(locked.pBits) + y * locked.Pitch;
      for (uint x = 0; x < desc.Width; x++, s += src_texel, dst += dst_texel) {
        device_texel_decode(device_format_, s, texel);
        texel_encode(texel, format, components, normalized, dst);
      }
    }
    if (from_staging) {
      readable->UnlockRect();
    }
    else {
      tex2d_->UnlockRect(mip);
    }
  }
  if (readable) {
    readable->Release();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Other operations
 * \{ */

void D3D9Texture::generate_mipmap()
{
  if (source_ || device_levels_ <= 1 || tex2d_ == nullptr || is_render_target_) {
    return;
  }
  if (device_format_.kind != D3D9TexelKind::UNORM8) {
    return;
  }
  /* Box filter each level from the previous one. */
  const int texel = d3d9_texel_size(device_format_);
  for (int level = 1; level < device_levels_; level++) {
    D3DLOCKED_RECT src, dst;
    D3DSURFACE_DESC src_desc, dst_desc;
    tex2d_->GetLevelDesc(level - 1, &src_desc);
    tex2d_->GetLevelDesc(level, &dst_desc);
    if (FAILED(tex2d_->LockRect(level - 1, &src, nullptr, D3DLOCK_READONLY))) {
      return;
    }
    if (FAILED(tex2d_->LockRect(level, &dst, nullptr, 0))) {
      tex2d_->UnlockRect(level - 1);
      return;
    }
    for (uint y = 0; y < dst_desc.Height; y++) {
      const uint y0 = min_uu(y * 2, src_desc.Height - 1);
      const uint y1 = min_uu(y * 2 + 1, src_desc.Height - 1);
      const uint8_t *row0 = static_cast<const uint8_t *>(src.pBits) + y0 * src.Pitch;
      const uint8_t *row1 = static_cast<const uint8_t *>(src.pBits) + y1 * src.Pitch;
      uint8_t *d = static_cast<uint8_t *>(dst.pBits) + y * dst.Pitch;
      for (uint x = 0; x < dst_desc.Width; x++) {
        const uint x0 = min_uu(x * 2, src_desc.Width - 1) * texel;
        const uint x1 = min_uu(x * 2 + 1, src_desc.Width - 1) * texel;
        for (int c = 0; c < texel; c++) {
          d[x * texel + c] = uint8_t(
              (uint(row0[x0 + c]) + row0[x1 + c] + row1[x0 + c] + row1[x1 + c] + 2) / 4);
        }
      }
    }
    tex2d_->UnlockRect(level);
    tex2d_->UnlockRect(level - 1);
  }
}

void D3D9Texture::copy_to(Texture *dst_, IndexRange mip_levels)
{
  D3D9Texture *dst = static_cast<D3D9Texture *>(dst_);
  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  for (const int mip : mip_levels) {
    if (is_render_target_ && dst->ensure_resource(true) && dst->is_render_target_ &&
        !is_depth() && !dst->is_depth())
    {
      IDirect3DSurface9 *src_surface = surface_get(mip, 0);
      IDirect3DSurface9 *dst_surface = dst->surface_get(mip, 0);
      if (src_surface && dst_surface) {
        D3D9_CHECK(device->StretchRect(
            src_surface, nullptr, dst_surface, nullptr, D3DTEXF_NONE));
      }
      d3d9_release(src_surface);
      d3d9_release(dst_surface);
      continue;
    }
    /* Slow path through host memory. */
    const size_t size = read_size_get(mip, GPU_DATA_FLOAT);
    float *data = static_cast<float *>(MEM_new_uninitialized(size, __func__));
    this->read(mip, GPU_DATA_FLOAT, data);
    int extent[3] = {1, 1, 1};
    mip_size_get(mip, extent);
    int offset[3] = {0, 0, 0};
    dst->update_sub(mip, offset, extent, GPU_DATA_FLOAT, data, 0);
    MEM_delete_void(static_cast<void *>(data));
  }
}

void D3D9Texture::swizzle_set(const char /*swizzle_mask*/[4])
{
  /* Not supported, single channel textures are stored as luminance which matches the common
   * "rrr1" swizzle. */
}

void D3D9Texture::mip_range_set(int min, int max)
{
  mip_min_ = min;
  mip_max_ = max;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pixel buffer
 * \{ */

D3D9PixelBuffer::D3D9PixelBuffer(size_t size) : PixelBuffer(size)
{
  data_ = MEM_new_uninitialized(size, __func__);
}

D3D9PixelBuffer::~D3D9PixelBuffer()
{
  MEM_delete_void(data_);
}

GPUPixelBufferNativeHandle D3D9PixelBuffer::get_native_handle()
{
  GPUPixelBufferNativeHandle handle;
  handle.handle = 0;
  handle.size = size_;
  return handle;
}

/** \} */

}  // namespace blender::gpu
