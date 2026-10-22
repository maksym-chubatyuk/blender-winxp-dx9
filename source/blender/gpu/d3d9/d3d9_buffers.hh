/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Buffers SM2.0 shaders cannot read directly. Their content lives in host memory:
 * - Uniform buffers are copied to constant registers when a shader declares a constant with the
 *   name of the block.
 * - Storage buffers only exist for the CPU side users (indirect draws, read-backs).
 */

#pragma once

#include "BLI_vector.hh"

#include "gpu_query.hh"
#include "gpu_state_private.hh"
#include "gpu_storage_buffer_private.hh"
#include "gpu_uniform_buffer_private.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

class D3D9UniformBuf : public UniformBuf {
 public:
  D3D9UniformBuf(size_t size, const char *name);
  ~D3D9UniformBuf() override;

  void update(const void *data) override;
  void clear_to_zero() override;
  void bind(int slot) override;
  void bind_as_ssbo(int /*slot*/) override {}
  void unbind() override;

  const void *data_get() const
  {
    return host_data_.data();
  }
  size_t size_get() const
  {
    return size_in_bytes_;
  }

 private:
  /** Data is owned by the base class only when attached, keep our own copy. */
  Vector<uint8_t> host_data_;
  int slot_ = -1;
};

class D3D9StorageBuf : public StorageBuf {
 public:
  D3D9StorageBuf(size_t size, const char *name);
  ~D3D9StorageBuf() override;

  void update(const void *data) override;
  void bind(int slot) override;
  void unbind() override;
  void clear(uint32_t clear_value) override;
  void copy_sub(VertBuf *src, uint dst_offset, uint src_offset, uint copy_size) override;
  void read(void *data) override;
  void async_flush_to_host() override {}
  void sync_as_indirect_buffer() override {}

  const uint8_t *data_get() const
  {
    return host_data_.data();
  }
  /** Data written by the CPU in place of shaders (selection). */
  uint8_t *data_get_mutable()
  {
    vbo_dirty_ = true;
    return host_data_.data();
  }
  size_t size_get() const
  {
    return host_data_.size();
  }

  /** The data as a vertex buffer, for shaders reading their vertices from the buffer
   * (`d3d9-vertex-pull` annotation). Uploaded again after each change. */
  IDirect3DVertexBuffer9 *vertex_buffer_get();

 private:
  Vector<uint8_t> host_data_;
  int slot_ = -1;
  IDirect3DVertexBuffer9 *vbo_ = nullptr;
  size_t vbo_size_ = 0;
  bool vbo_dirty_ = true;
};

class D3D9QueryPool : public QueryPool {
 public:
  ~D3D9QueryPool() override;

  void init(GPUQueryType type) override;
  void begin_query() override;
  void end_query() override;
  void get_occlusion_result(MutableSpan<uint32_t> r_values) override;

 private:
  Vector<IDirect3DQuery9 *> queries_;
  IDirect3DQuery9 *active_ = nullptr;
};

class D3D9Fence : public Fence {
 public:
  ~D3D9Fence() override;

  void signal() override;
  void wait() override;

 private:
  IDirect3DQuery9 *query_ = nullptr;
};

}  // namespace blender::gpu
