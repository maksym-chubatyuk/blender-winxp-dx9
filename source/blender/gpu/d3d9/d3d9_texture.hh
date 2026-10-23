/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_texture_private.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

/** How texels are stored on the device, see #d3d9_texture_format. */
enum class D3D9TexelKind : uint8_t {
  /** 8 bit normalized, stored as BGRA (`D3DFMT_A8R8G8B8`) or L (`D3DFMT_L8`). */
  UNORM8,
  /** 16 bit float per component. */
  HALF,
  /** 32 bit float per component. */
  FLOAT,
  /** Block compressed, uploaded as is. */
  COMPRESSED,
  /**
   * 32 bit unsigned integer stored as the 4 bytes of a `D3DFMT_A8R8G8B8` texel: exact with the
   * 24 bit floats of SM2.0 pixel shaders, which write the bytes (see `select_id.hlsl`).
   */
  PACKED_U32,
  /** Depth (stencil) surface, not sampled. */
  DEPTH,
};

struct D3D9TextureFormat {
  D3DFORMAT format = D3DFMT_UNKNOWN;
  D3D9TexelKind kind = D3D9TexelKind::UNORM8;
  /** Number of components stored on the device (1, 2 or 4). */
  int components = 4;
};

/** Device format used for a Blender texture format. */
D3D9TextureFormat d3d9_texture_format(TextureFormat format);

class D3D9Texture : public Texture {
  friend class D3D9StateManager;
  friend class D3D9FrameBuffer;

 public:
  explicit D3D9Texture(const char *name);
  ~D3D9Texture() override;

  void generate_mipmap() override;
  void copy_to(Texture *dst, IndexRange mip_levels) override;
  void clear(double4 data) override;
  void swizzle_set(const char swizzle_mask[4]) override;
  void mip_range_set(int min, int max) override;
  void read(int mip, eGPUDataFormat format, void *r_data) override;

  void update_sub(int mip,
                  int offset[3],
                  int extent[3],
                  eGPUDataFormat format,
                  const void *data,
                  uint unpack_row_length = 0) override;
  void update_sub(int offset[3],
                  int extent[3],
                  eGPUDataFormat format,
                  GPUPixelBuffer *pixbuf) override;

  /** Texture to sample from, created on first use. Null for depth textures. */
  IDirect3DBaseTexture9 *base_texture_get();
  /** Clear with `ColorFill`, false when it isn't possible (see #clear). */
  bool clear_on_device(const double4 &data);
  /** Surface of a mip level to render into, the texture becomes a render target. */
  IDirect3DSurface9 *surface_get(int mip, int layer);

  const D3D9TextureFormat &device_format() const
  {
    return device_format_;
  }
  bool is_depth() const
  {
    return device_format_.kind == D3D9TexelKind::DEPTH;
  }
  /** Device size of the texture, it can differ from the requested size, see #init_internal. */
  int device_width() const
  {
    return device_w_;
  }
  int device_height() const
  {
    return device_h_;
  }
  const char *debug_name() const
  {
    return name_.c_str();
  }
  bool is_render_target() const
  {
    return source_ ? source_->is_render_target_ : is_render_target_;
  }
  /** Float formats can't be filtered by most SM2.0 hardware. */
  bool is_filterable() const
  {
    return filterable_;
  }
  /** Highest mip level sampling can use. */
  int device_levels() const
  {
    return device_levels_;
  }

 protected:
  bool init_internal() override;
  bool init_internal(VertBuf *vbo) override;
  bool init_internal(gpu::Texture *src, int mip_offset, int layer_offset, bool use_stencil) override;

 private:
  bool ensure_resource(bool render_target);
  void free_resource();
  /** Copy the content of a managed texture into a render target one. */
  bool convert_to_render_target();
  /** Write host data into a locked rectangle of device texels. */
  void convert_upload(const void *src,
                      eGPUDataFormat src_format,
                      int width,
                      int height,
                      int src_row_len,
                      uint8_t *dst,
                      int dst_pitch) const;
  IDirect3DSurface9 *level_surface(int mip, int layer);

  D3D9TextureFormat device_format_;
  int device_w_ = 0, device_h_ = 0, device_d_ = 0;
  int device_levels_ = 1;

  IDirect3DTexture9 *tex2d_ = nullptr;
  IDirect3DCubeTexture9 *cube_ = nullptr;
  IDirect3DVolumeTexture9 *volume_ = nullptr;
  /** Depth formats are depth-stencil surfaces. */
  IDirect3DSurface9 *depth_surface_ = nullptr;
  bool is_render_target_ = false;
  bool filterable_ = true;
  /** The device refused to create the texture: don't try again on every use. */
  bool creation_failed_ = false;
  /** Video memory of the render target or depth surface. */
  int64_t vram_bytes_ = 0;
  /** Texture views share the resource of their source. */
  D3D9Texture *source_ = nullptr;
  int view_mip_offset_ = 0;
};

class D3D9PixelBuffer : public PixelBuffer {
 public:
  explicit D3D9PixelBuffer(size_t size);
  ~D3D9PixelBuffer() override;

  void *map() override
  {
    return data_;
  }
  void unmap() override {}
  GPUPixelBufferNativeHandle get_native_handle() override;
  size_t get_size() override
  {
    return size_;
  }

 private:
  void *data_ = nullptr;
};

}  // namespace blender::gpu
