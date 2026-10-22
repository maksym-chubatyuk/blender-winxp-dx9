/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "d3d9_backend.hh"
#include "d3d9_index_buffer.hh"

namespace blender::gpu {

/** Versions are unique over all the buffers: a new buffer can have the address of a freed one. */
static uint32_t next_version()
{
  static uint32_t version = 0;
  return ++version;
}

D3D9IndexBuf::~D3D9IndexBuf()
{
  d3d9_release(ibo_);
}

IDirect3DIndexBuffer9 *D3D9IndexBuf::buffer_get()
{
  if (is_subrange_) {
    return static_cast<D3D9IndexBuf *>(src_)->buffer_get();
  }
  if (ibo_ != nullptr && data_ == nullptr) {
    return ibo_;
  }
  const size_t size = max_zz(this->size_get(), 4);
  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  if (ibo_ == nullptr) {
    if (!D3D9_CHECK(device->CreateIndexBuffer(size,
                                              D3DUSAGE_WRITEONLY,
                                              is_32bit() ? D3DFMT_INDEX32 : D3DFMT_INDEX16,
                                              D3DPOOL_MANAGED,
                                              &ibo_,
                                              nullptr)))
    {
      return nullptr;
    }
  }
  void *dst;
  if (SUCCEEDED(ibo_->Lock(0, size, &dst, 0))) {
    if (data_) {
      memcpy(dst, data_, this->size_get());
    }
    else {
      memset(dst, 0, size);
    }
    ibo_->Unlock();
  }
  /* No need to keep a copy of data in system memory. */
  MEM_SAFE_DELETE_VOID(data_);
  version_ = next_version();
  return ibo_;
}

void D3D9IndexBuf::upload_data()
{
  buffer_get();
}

void D3D9IndexBuf::read(uint32_t *data) const
{
  if (ibo_ == nullptr) {
    return;
  }
  void *src;
  if (SUCCEEDED(ibo_->Lock(0, 0, &src, D3DLOCK_READONLY))) {
    memcpy(data, src, size_get());
    ibo_->Unlock();
  }
}

void D3D9IndexBuf::update_sub(uint start, uint len, const void *data)
{
  if (buffer_get() == nullptr) {
    return;
  }
  void *dst;
  if (SUCCEEDED(ibo_->Lock(start, len, &dst, 0))) {
    memcpy(dst, data, len);
    ibo_->Unlock();
  }
  version_ = next_version();
}

Span<uint32_t> D3D9IndexBuf::host_indices_get()
{
  if (is_subrange_) {
    return source_get()->host_indices_get();
  }
  if (buffer_get() == nullptr) {
    return {};
  }
  if (host_indices_version_ == version_) {
    return host_indices_;
  }
  host_indices_version_ = version_;
  host_indices_.resize(index_len_);
  void *src;
  /* Managed buffers are read from their system memory copy. */
  if (FAILED(ibo_->Lock(0, 0, &src, D3DLOCK_READONLY))) {
    host_indices_.clear();
    return {};
  }
  for (uint i = 0; i < index_len_; i++) {
    const uint32_t index = is_32bit() ? static_cast<const uint32_t *>(src)[i] :
                                        static_cast<const uint16_t *>(src)[i];
    host_indices_[i] = index + index_base_;
  }
  ibo_->Unlock();
  return host_indices_;
}

void D3D9IndexBuf::strip_restart_indices()
{
  /* Only called for points: restart indices are replaced by a repeated valid index. */
  if (data_ == nullptr || index_len_ == 0) {
    return;
  }
  uint32_t *indices = static_cast<uint32_t *>(data_);
  uint32_t valid = 0;
  for (uint i = 0; i < index_len_; i++) {
    if (indices[i] != RESTART_INDEX) {
      valid = indices[i];
      break;
    }
  }
  for (uint i = 0; i < index_len_; i++) {
    if (indices[i] == RESTART_INDEX) {
      indices[i] = valid;
    }
  }
}

}  // namespace blender::gpu
