/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_vertex_buffer.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

class D3D9IndexBuf;

/**
 * Device side layout of a vertex format.
 *
 * SM2.0 hardware only reliably fetches floats and `D3DCOLOR`: every other attribute type is
 * converted to floats on upload, and de-interleaved formats are interleaved.
 */
struct D3D9VertexLayout {
  struct Attr {
    /** `D3DDECLTYPE`. */
    uint8_t type;
    uint8_t offset;
  };
  Attr attrs[GPU_VERT_ATTR_MAX_LEN];
  uint stride = 0;
  /** True when the device data is the host data as is. */
  bool is_identity = false;

  void init(const GPUVertFormat &format);
  /** Convert `vert_len` vertices from host to device layout. */
  void convert(const GPUVertFormat &format, const void *src, void *dst, uint vert_len) const;
};

class D3D9VertBuf : public VertBuf {
 public:
  ~D3D9VertBuf() override;

  void bind_as_ssbo(uint /*binding*/) override {}
  void bind_as_texture(uint /*binding*/) override {}
  void wrap_handle(uint64_t /*handle*/) override {}

  void update_sub(uint start, uint len, const void *data) override;
  void read(void *data) const override;

  /** Upload if needed and return the device buffer (null when empty). */
  IDirect3DVertexBuffer9 *buffer_get();
  const D3D9VertexLayout &layout_get()
  {
    if (layout_dirty_) {
      layout_.init(format);
      layout_dirty_ = false;
    }
    return layout_;
  }
  /**
   * Buffer with the vertices of the indices `start` to `start + count` of `ib`, in order (null on
   * failure). Kept until the vertices, the indices or the range change.
   */
  IDirect3DVertexBuffer9 *deindexed_get(D3D9IndexBuf &ib, uint start, uint count);
  /** Incremented when the device data changes. */
  uint32_t version_get() const
  {
    return version_;
  }
  /** Host data, null once a static buffer is uploaded. */
  const uchar *host_data_get() const
  {
    return data_;
  }

 protected:
  void acquire_data() override;
  void resize_data() override;
  void release_data() override;
  void upload_data() override;

 private:
  IDirect3DVertexBuffer9 *vbo_ = nullptr;
  uint vbo_size_ = 0;
  D3D9VertexLayout layout_;
  bool layout_dirty_ = true;
  /** Incremented when the device data changes. */
  uint32_t version_ = 1;

  IDirect3DVertexBuffer9 *deindexed_ = nullptr;
  uint deindexed_size_ = 0;
  const D3D9IndexBuf *deindexed_ib_ = nullptr;
  uint32_t deindexed_ib_version_ = 0;
  uint32_t deindexed_version_ = 0;
  uint deindexed_start_ = 0;
  uint deindexed_count_ = 0;
};

}  // namespace blender::gpu
