/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstring>

#include "BLI_math_base.h"

#include "MEM_guardedalloc.h"

#include "d3d9_backend.hh"
#include "d3d9_buffers.hh"
#include "d3d9_context.hh"
#include "d3d9_vertex_buffer.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Uniform buffer
 * \{ */

D3D9UniformBuf::D3D9UniformBuf(size_t size, const char *name) : UniformBuf(size, name)
{
  host_data_.resize(size, 0);
}

D3D9UniformBuf::~D3D9UniformBuf()
{
  this->unbind();
}

void D3D9UniformBuf::update(const void *data)
{
  memcpy(host_data_.data(), data, size_in_bytes_);
}

void D3D9UniformBuf::clear_to_zero()
{
  memset(host_data_.data(), 0, size_in_bytes_);
}

void D3D9UniformBuf::bind(int slot)
{
  if (slot < 0 || slot >= D3D9_MAX_UBO_SLOTS) {
    return;
  }
  if (data_ != nullptr) {
    /* Data attached by #GPU_uniformbuf_create_ex is uploaded on first bind. */
    memcpy(host_data_.data(), data_, size_in_bytes_);
    MEM_SAFE_DELETE_VOID(data_);
  }
  D3D9Context *ctx = D3D9Context::get();
  ctx->bound_ubos[slot] = this;
  slot_ = slot;
}

void D3D9UniformBuf::unbind()
{
  D3D9Context *ctx = D3D9Context::get();
  if (ctx && slot_ >= 0 && ctx->bound_ubos[slot_] == this) {
    ctx->bound_ubos[slot_] = nullptr;
  }
  slot_ = -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Storage buffer
 * \{ */

D3D9StorageBuf::D3D9StorageBuf(size_t size, const char *name) : StorageBuf(size, name)
{
  host_data_.resize(size, 0);
}

D3D9StorageBuf::~D3D9StorageBuf() = default;

void D3D9StorageBuf::update(const void *data)
{
  memcpy(host_data_.data(), data, min_zz(usage_size_in_bytes_, host_data_.size()));
}

void D3D9StorageBuf::clear(uint32_t clear_value)
{
  uint32_t *data = reinterpret_cast<uint32_t *>(host_data_.data());
  for (size_t i = 0; i < host_data_.size() / 4; i++) {
    data[i] = clear_value;
  }
}

void D3D9StorageBuf::copy_sub(VertBuf *src, uint dst_offset, uint src_offset, uint copy_size)
{
  D3D9VertBuf *vbo = static_cast<D3D9VertBuf *>(src);
  const uchar *data = vbo->host_data_get();
  if (data == nullptr || dst_offset + copy_size > host_data_.size()) {
    return;
  }
  memcpy(host_data_.data() + dst_offset, data + src_offset, copy_size);
}

void D3D9StorageBuf::read(void *data)
{
  memcpy(data, host_data_.data(), min_zz(usage_size_in_bytes_, host_data_.size()));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Queries
 * \{ */

D3D9QueryPool::~D3D9QueryPool()
{
  for (IDirect3DQuery9 *query : queries_) {
    query->Release();
  }
}

void D3D9QueryPool::init(GPUQueryType /*type*/) {}

void D3D9QueryPool::begin_query()
{
  D3D9Context *ctx = D3D9Context::get();
  IDirect3DQuery9 *query = nullptr;
  if (FAILED(ctx->device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query))) {
    /* Unsupported: results will report everything as visible. */
    queries_.append(nullptr);
    active_ = nullptr;
    return;
  }
  ctx->ensure_scene();
  query->Issue(D3DISSUE_BEGIN);
  queries_.append(query);
  active_ = query;
}

void D3D9QueryPool::end_query()
{
  if (active_) {
    active_->Issue(D3DISSUE_END);
    active_ = nullptr;
  }
}

void D3D9QueryPool::get_occlusion_result(MutableSpan<uint32_t> r_values)
{
  D3D9Context *ctx = D3D9Context::get();
  ctx->end_scene();
  for (int i = 0; i < r_values.size(); i++) {
    IDirect3DQuery9 *query = i < queries_.size() ? queries_[i] : nullptr;
    if (query == nullptr) {
      r_values[i] = 1;
      continue;
    }
    DWORD pixels = 0;
    while (query->GetData(&pixels, sizeof(pixels), D3DGETDATA_FLUSH) == S_FALSE) {
      Sleep(0);
    }
    r_values[i] = uint32_t(pixels);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Fence
 * \{ */

D3D9Fence::~D3D9Fence()
{
  d3d9_release(query_);
}

void D3D9Fence::signal()
{
  D3D9Context *ctx = D3D9Context::get();
  if (query_ == nullptr) {
    ctx->device->CreateQuery(D3DQUERYTYPE_EVENT, &query_);
  }
  if (query_) {
    query_->Issue(D3DISSUE_END);
  }
  signalled_ = true;
}

void D3D9Fence::wait()
{
  if (!signalled_ || query_ == nullptr) {
    return;
  }
  while (query_->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
    Sleep(0);
  }
  signalled_ = false;
}

/** \} */

}  // namespace blender::gpu
