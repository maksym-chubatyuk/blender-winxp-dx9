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
#include "d3d9_vertex_buffer.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Device layout
 * \{ */

void D3D9VertexLayout::init(const GPUVertFormat &format)
{
  stride = 0;
  is_identity = !format.deinterleaved;
  for (uint a_idx = 0; a_idx < format.attr_len; a_idx++) {
    const GPUVertAttr &a = format.attrs[a_idx];
    const int comp_len = a.type.comp_len();
    uint8_t type;
    uint size;
    switch (a.type.format) {
      case VertAttrType::SFLOAT_32:
        type = D3DDECLTYPE_FLOAT1;
        size = 4;
        break;
      case VertAttrType::SFLOAT_32_32:
        type = D3DDECLTYPE_FLOAT2;
        size = 8;
        break;
      case VertAttrType::SFLOAT_32_32_32:
        type = D3DDECLTYPE_FLOAT3;
        size = 12;
        break;
      case VertAttrType::SFLOAT_32_32_32_32:
        type = D3DDECLTYPE_FLOAT4;
        size = 16;
        break;
      case VertAttrType::UNORM_8_8_8_8:
        /* Swizzled to BGRA on upload. */
        type = D3DDECLTYPE_D3DCOLOR;
        size = 4;
        is_identity = false;
        break;
      default:
        /* Everything else is converted to floats. */
        type = uint8_t(D3DDECLTYPE_FLOAT1 + clamp_i(comp_len, 1, 4) - 1);
        size = 4 * clamp_i(comp_len, 1, 4);
        is_identity = false;
        break;
    }
    if (a.offset != stride) {
      is_identity = false;
    }
    attrs[a_idx].type = type;
    attrs[a_idx].offset = uint8_t(stride);
    stride += size;
  }
  if (stride != format.stride) {
    is_identity = false;
  }
}

/** Read the components of one attribute as floats. */
static void attr_decode(VertAttrType type, const uint8_t *src, float r_value[4])
{
  r_value[0] = r_value[1] = r_value[2] = 0.0f;
  r_value[3] = 1.0f;
  switch (type) {
#define CASE_INT(enum_type, c_type, comps, norm) \
  case VertAttrType::enum_type: { \
    c_type v[comps]; \
    memcpy(v, src, sizeof(v)); \
    for (int c = 0; c < comps; c++) { \
      r_value[c] = (norm != 0.0f) ? max_ff(float(v[c]) / norm, -1.0f) : float(v[c]); \
    } \
    break; \
  }
    CASE_INT(SNORM_8_8_8_8, int8_t, 4, 127.0f)
    CASE_INT(SNORM_16_16, int16_t, 2, 32767.0f)
    CASE_INT(SNORM_16_16_16_16, int16_t, 4, 32767.0f)
    CASE_INT(UNORM_8_8_8_8, uint8_t, 4, 255.0f)
    CASE_INT(UNORM_16_16, uint16_t, 2, 65535.0f)
    CASE_INT(UNORM_16_16_16_16, uint16_t, 4, 65535.0f)
    CASE_INT(SINT_8_8_8_8, int8_t, 4, 0.0f)
    CASE_INT(SINT_16_16, int16_t, 2, 0.0f)
    CASE_INT(SINT_16_16_16_16, int16_t, 4, 0.0f)
    CASE_INT(SINT_32, int32_t, 1, 0.0f)
    CASE_INT(SINT_32_32, int32_t, 2, 0.0f)
    CASE_INT(SINT_32_32_32, int32_t, 3, 0.0f)
    CASE_INT(SINT_32_32_32_32, int32_t, 4, 0.0f)
    CASE_INT(UINT_8_8_8_8, uint8_t, 4, 0.0f)
    CASE_INT(UINT_16_16, uint16_t, 2, 0.0f)
    CASE_INT(UINT_16_16_16_16, uint16_t, 4, 0.0f)
    CASE_INT(UINT_32, uint32_t, 1, 0.0f)
    CASE_INT(UINT_32_32, uint32_t, 2, 0.0f)
    CASE_INT(UINT_32_32_32, uint32_t, 3, 0.0f)
    CASE_INT(UINT_32_32_32_32, uint32_t, 4, 0.0f)
    CASE_INT(SFLOAT_32, float, 1, 0.0f)
    CASE_INT(SFLOAT_32_32, float, 2, 0.0f)
    CASE_INT(SFLOAT_32_32_32, float, 3, 0.0f)
    CASE_INT(SFLOAT_32_32_32_32, float, 4, 0.0f)
#undef CASE_INT
    case VertAttrType::SNORM_10_10_10_2: {
      uint32_t v;
      memcpy(&v, src, 4);
      auto snorm10 = [](uint32_t bits) {
        const int value = int(bits << 22) >> 22;
        return max_ff(float(value) / 511.0f, -1.0f);
      };
      r_value[0] = snorm10(v & 0x3FF);
      r_value[1] = snorm10((v >> 10) & 0x3FF);
      r_value[2] = snorm10((v >> 20) & 0x3FF);
      r_value[3] = float(int(v) >> 30);
      break;
    }
    case VertAttrType::UNORM_10_10_10_2: {
      uint32_t v;
      memcpy(&v, src, 4);
      r_value[0] = float(v & 0x3FF) / 1023.0f;
      r_value[1] = float((v >> 10) & 0x3FF) / 1023.0f;
      r_value[2] = float((v >> 20) & 0x3FF) / 1023.0f;
      r_value[3] = float(v >> 30) / 3.0f;
      break;
    }
    default:
      break;
  }
}

void D3D9VertexLayout::convert(const GPUVertFormat &format,
                               const void *src_data,
                               void *dst_data,
                               uint vert_len) const
{
  const uint8_t *src = static_cast<const uint8_t *>(src_data);
  uint8_t *dst = static_cast<uint8_t *>(dst_data);
  if (is_identity) {
    memcpy(dst, src, size_t(vert_len) * stride);
    return;
  }

  size_t deinterleaved_offset = 0;
  for (uint a_idx = 0; a_idx < format.attr_len; a_idx++) {
    const GPUVertAttr &a = format.attrs[a_idx];
    size_t src_offset, src_stride;
    if (format.deinterleaved) {
      if (a_idx > 0) {
        deinterleaved_offset += format.attrs[a_idx - 1].type.size() * vert_len;
      }
      src_offset = deinterleaved_offset;
      src_stride = a.type.size();
    }
    else {
      src_offset = a.offset;
      src_stride = format.stride;
    }
    const Attr &d = attrs[a_idx];
    const uint8_t *s = src + src_offset;
    uint8_t *t = dst + d.offset;

    if (d.type == D3DDECLTYPE_D3DCOLOR) {
      for (uint v = 0; v < vert_len; v++, s += src_stride, t += stride) {
        t[0] = s[2];
        t[1] = s[1];
        t[2] = s[0];
        t[3] = s[3];
      }
      continue;
    }
    const int comps = d.type - D3DDECLTYPE_FLOAT1 + 1;
    const bool is_float = ELEM(a.type.format,
                               VertAttrType::SFLOAT_32,
                               VertAttrType::SFLOAT_32_32,
                               VertAttrType::SFLOAT_32_32_32,
                               VertAttrType::SFLOAT_32_32_32_32);
    if (is_float) {
      for (uint v = 0; v < vert_len; v++, s += src_stride, t += stride) {
        memcpy(t, s, comps * sizeof(float));
      }
      continue;
    }
    float value[4];
    for (uint v = 0; v < vert_len; v++, s += src_stride, t += stride) {
      attr_decode(a.type.format, s, value);
      memcpy(t, value, comps * sizeof(float));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex buffer
 * \{ */

D3D9VertBuf::~D3D9VertBuf()
{
  this->release_data();
}

void D3D9VertBuf::acquire_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }
  MEM_SAFE_DELETE(data_);
  data_ = MEM_new_array_uninitialized<uchar>(this->size_alloc_get(), __func__);
  layout_dirty_ = true;
}

void D3D9VertBuf::resize_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }
  data_ = static_cast<uchar *>(
      MEM_realloc_uninitialized(data_, sizeof(uchar) * this->size_alloc_get()));
}

void D3D9VertBuf::release_data()
{
  if (vbo_) {
    memory_usage -= vbo_size_;
    d3d9_release(vbo_);
    vbo_size_ = 0;
  }
  MEM_SAFE_DELETE(data_);
}

void D3D9VertBuf::upload_data()
{
  buffer_get();
}

IDirect3DVertexBuffer9 *D3D9VertBuf::buffer_get()
{
  if (vbo_ != nullptr && !(flag & GPU_VERTBUF_DATA_DIRTY)) {
    return vbo_;
  }
  if (vertex_len == 0) {
    return nullptr;
  }
  const D3D9VertexLayout &layout = layout_get();
  const uint size = max_uu(layout.stride * vertex_len, 4);
  IDirect3DDevice9 *device = D3D9Backend::get()->device;

  if (vbo_ == nullptr || vbo_size_ < size) {
    if (vbo_) {
      memory_usage -= vbo_size_;
    }
    d3d9_release(vbo_);
    if (!D3D9_CHECK(device->CreateVertexBuffer(
            size, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vbo_, nullptr)))
    {
      vbo_size_ = 0;
      return nullptr;
    }
    vbo_size_ = size;
    memory_usage += vbo_size_;
  }

  void *dst;
  if (SUCCEEDED(vbo_->Lock(0, size, &dst, 0))) {
    if (data_ != nullptr && usage_ != GPU_USAGE_DEVICE_ONLY) {
      layout.convert(format, data_, dst, vertex_len);
    }
    else {
      memset(dst, 0, size);
    }
    vbo_->Unlock();
  }

  if (usage_ == GPU_USAGE_STATIC) {
    MEM_SAFE_DELETE(data_);
  }
  flag &= ~GPU_VERTBUF_DATA_DIRTY;
  flag |= GPU_VERTBUF_DATA_UPLOADED;
  return vbo_;
}

void D3D9VertBuf::update_sub(uint start, uint len, const void *data)
{
  const D3D9VertexLayout &layout = layout_get();
  if (layout.is_identity) {
    if (buffer_get() == nullptr || start + len > vbo_size_) {
      return;
    }
    void *dst;
    if (SUCCEEDED(vbo_->Lock(start, len, &dst, 0))) {
      memcpy(dst, data, len);
      vbo_->Unlock();
    }
    return;
  }
  /* Converted layouts: patch the host data and upload everything again. */
  if (data_ == nullptr || start + len > size_alloc_get()) {
    return;
  }
  memcpy(data_ + start, data, len);
  flag |= GPU_VERTBUF_DATA_DIRTY;
  buffer_get();
}

void D3D9VertBuf::read(void *data) const
{
  const size_t size = size_used_get();
  if (data_) {
    memcpy(data, data_, size);
    return;
  }
  if (vbo_ && layout_.is_identity && !layout_dirty_) {
    void *src;
    if (SUCCEEDED(vbo_->Lock(0, 0, &src, D3DLOCK_READONLY))) {
      memcpy(data, src, min_zz(size, vbo_size_));
      vbo_->Unlock();
      return;
    }
  }
  memset(data, 0, size);
}

/** \} */

}  // namespace blender::gpu
