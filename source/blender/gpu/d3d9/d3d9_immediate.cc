/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstring>

#include "GPU_shader.hh"

#include "gpu_shader_interface.hh"
#include "gpu_vertex_format_private.hh"

#include "d3d9_backend.hh"
#include "d3d9_batch.hh"
#include "d3d9_context.hh"
#include "d3d9_immediate.hh"
#include "d3d9_shader.hh"
#include "d3d9_vertex_buffer.hh"

namespace blender::gpu {

/** Size of the streaming vertex buffer. */

D3D9Immediate::~D3D9Immediate()
{
  d3d9_release(vbo_);
}

uchar *D3D9Immediate::begin()
{
  /* One extra vertex to close line loops. */
  const size_t bytes_needed = vertex_buffer_size(&vertex_format, vertex_len + 1);
  if (host_data_.size() < int64_t(bytes_needed)) {
    host_data_.resize(bytes_needed);
  }
  return host_data_.data();
}

void D3D9Immediate::end()
{
  BLI_assert(prim_type != GPU_PRIM_NONE);
  uint vert_len = strict_vertex_len ? vertex_len : vertex_idx;
  if (vert_len == 0) {
    return;
  }

  D3D9Context *ctx = D3D9Context::get();
  /* Update matrices. */
  GPU_shader_bind(shader);
  if (!ctx->draw_setup()) {
    return;
  }
  D3D9Shader *d3d9_shader = static_cast<D3D9Shader *>(ctx->shader);
  IDirect3DDevice9 *device = ctx->device;

  GPUPrimType draw_prim = prim_type;
  if (prim_type == GPU_PRIM_LINE_LOOP && vert_len > 2) {
    /* Close the loop by repeating the first vertex. */
    memcpy(host_data_.data() + vert_len * vertex_format.stride,
           host_data_.data(),
           vertex_format.stride);
    vert_len++;
    draw_prim = GPU_PRIM_LINE_STRIP;
  }
  /* Points drawn as quads: every vertex is repeated for the 6 corners of its 2 triangles. */
  const bool point_quads = draw_prim == GPU_PRIM_POINTS && d3d9_shader->point_quads();
  const uint repeat = point_quads ? 6 : 1;
  D3DPRIMITIVETYPE type;
  uint prim_len;
  if (!d3d9_primitive(point_quads ? GPU_PRIM_TRIS : draw_prim, vert_len * repeat, type, prim_len))
  {
    return;
  }
  ctx->point_sprite_set(type == D3DPT_POINTLIST && d3d9_shader->point_sprite());
  ctx->flat_color_set(d3d9_shader->flat_color());

  D3D9VertexLayout layout;
  layout.init(vertex_format);
  const uint size = layout.stride * vert_len * repeat;
  if (size > D3D9_IMM_BUFFER_SIZE) {
    return;
  }

  /* Stream the vertices: append to the buffer, start over when full. */
  if (vbo_ == nullptr) {
    if (!D3D9_CHECK(device->CreateVertexBuffer(D3D9_IMM_BUFFER_SIZE,
                                               D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                               0,
                                               D3DPOOL_DEFAULT,
                                               &vbo_,
                                               nullptr)))
    {
      return;
    }
    offset_ = D3D9_IMM_BUFFER_SIZE;
  }
  /* Vertices are addressed with the start vertex: align to the stride (and to the 6 corners of
   * the point quads, the corner stream uses the same start vertex). */
  const uint align = layout.stride * repeat;
  uint offset = ((offset_ + align - 1) / align) * align;
  DWORD lock_flags = D3DLOCK_NOOVERWRITE;
  if (offset + size > D3D9_IMM_BUFFER_SIZE) {
    offset = 0;
    lock_flags = D3DLOCK_DISCARD;
  }
  void *dst;
  if (FAILED(vbo_->Lock(offset, size, &dst, lock_flags))) {
    return;
  }
  if (point_quads) {
    Vector<uint8_t> converted(layout.stride * vert_len);
    layout.convert(vertex_format, host_data_.data(), converted.data(), vert_len);
    uint8_t *out = static_cast<uint8_t *>(dst);
    for (uint v = 0; v < vert_len; v++) {
      for (uint corner = 0; corner < 6; corner++, out += layout.stride) {
        memcpy(out, converted.data() + v * layout.stride, layout.stride);
      }
    }
  }
  else {
    layout.convert(vertex_format, host_data_.data(), dst, vert_len);
  }
  vbo_->Unlock();
  offset_ = offset + size;

  /* Vertex declaration: a single stream, plus the default attribute stream if needed. */
  D3D9VertexDeclKey key;
  const uint16_t inputs = d3d9_shader->vertex_inputs_get();
  uint16_t bound = 0;
  const ShaderInterface *interface = d3d9_shader->interface;
  for (uint a_idx = 0; a_idx < vertex_format.attr_len; a_idx++) {
    const GPUVertAttr &a = vertex_format.attrs[a_idx];
    for (uint n_idx = 0; n_idx < a.name_len; n_idx++) {
      const ShaderInput *input = interface->attr_get(
          GPU_vertformat_attr_name_get(&vertex_format, &a, n_idx));
      if (input == nullptr || input->location < 0) {
        continue;
      }
      const int loc = input->location;
      if (!(inputs & (1 << loc)) || (bound & (1 << loc))) {
        continue;
      }
      key.elements.append({0,
                           WORD(layout.attrs[a_idx].offset),
                           BYTE(layout.attrs[a_idx].type),
                           D3DDECLMETHOD_DEFAULT,
                           D3DDECLUSAGE_TEXCOORD,
                           BYTE(loc)});
      bound |= 1 << loc;
    }
  }
  int stream = 1;
  if (d3d9_shader->uses_vertex_id()) {
    key.elements.append({WORD(stream),
                         0,
                         D3DDECLTYPE_FLOAT1,
                         D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD,
                         BYTE(D3D9_VERTEX_ID_ATTR)});
    device->SetStreamSource(stream, D3D9Backend::get()->vertex_id_buffer(), 0, sizeof(float));
    stream++;
  }
  if (point_quads) {
    key.elements.append({WORD(stream),
                         0,
                         D3DDECLTYPE_FLOAT2,
                         D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD,
                         BYTE(D3D9_POINT_CORNER_ATTR)});
    device->SetStreamSource(
        stream, D3D9Backend::get()->point_corner_buffer(), 0, sizeof(float[2]));
    stream++;
  }
  const uint16_t missing = inputs & ~bound & ~(1 << D3D9_VERTEX_ID_ATTR) &
                           ~(1 << D3D9_POINT_CORNER_ATTR);
  if (missing) {
    for (int loc = 0; loc < 16; loc++) {
      if (missing & (1 << loc)) {
        key.elements.append({WORD(stream),
                             0,
                             D3DDECLTYPE_D3DCOLOR,
                             D3DDECLMETHOD_DEFAULT,
                             D3DDECLUSAGE_TEXCOORD,
                             BYTE(loc)});
      }
    }
    device->SetStreamSource(
        stream, D3D9Backend::get()->default_attr_buffer(), 0, sizeof(D3DCOLOR));
  }
  if (key.elements.is_empty()) {
    return;
  }
  IDirect3DVertexDeclaration9 *decl = ctx->vertex_declaration_get(key);
  if (decl == nullptr) {
    return;
  }
  device->SetVertexDeclaration(decl);
  device->SetStreamSource(0, vbo_, 0, layout.stride);
  const double gpu_start_ms = d3d9_gpu_time_enabled() ? d3d9_gpu_time_begin(device) : 0.0;
  const double draw_start_ms = d3d9_time_ms();
  device->DrawPrimitive(type, offset / layout.stride, prim_len);
  d3d9_frame_stats.draw_ms += d3d9_time_ms() - draw_start_ms;
  if (d3d9_gpu_time_enabled()) {
    d3d9_gpu_time_end(device, d3d9_shader->name_get().c_str(), gpu_start_ms);
  }
}

}  // namespace blender::gpu
