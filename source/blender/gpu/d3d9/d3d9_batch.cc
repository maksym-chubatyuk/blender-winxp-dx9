/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <algorithm>

#include "GPU_vertex_format.hh"

#include "gpu_shader_interface.hh"

#include <cstdio>
#include <cstdlib>

#include "d3d9_backend.hh"
#include "d3d9_batch.hh"
#include "d3d9_buffers.hh"
#include "d3d9_context.hh"
#include "d3d9_index_buffer.hh"
#include "d3d9_shader.hh"
#include "d3d9_vertex_buffer.hh"

namespace blender::gpu {

bool d3d9_primitive(GPUPrimType prim_type,
                    uint vert_len,
                    D3DPRIMITIVETYPE &r_type,
                    uint &r_prim_len)
{
  switch (prim_type) {
    case GPU_PRIM_POINTS:
      r_type = D3DPT_POINTLIST;
      r_prim_len = vert_len;
      break;
    case GPU_PRIM_LINES:
      r_type = D3DPT_LINELIST;
      r_prim_len = vert_len / 2;
      break;
    case GPU_PRIM_LINE_STRIP:
    case GPU_PRIM_LINE_LOOP:
      r_type = D3DPT_LINESTRIP;
      r_prim_len = vert_len > 1 ? vert_len - 1 : 0;
      break;
    case GPU_PRIM_TRIS:
      r_type = D3DPT_TRIANGLELIST;
      r_prim_len = vert_len / 3;
      break;
    case GPU_PRIM_TRI_STRIP:
      r_type = D3DPT_TRIANGLESTRIP;
      r_prim_len = vert_len > 2 ? vert_len - 2 : 0;
      break;
    case GPU_PRIM_TRI_FAN:
      r_type = D3DPT_TRIANGLEFAN;
      r_prim_len = vert_len > 2 ? vert_len - 2 : 0;
      break;
    default:
      /* Adjacency primitives need geometry shaders. */
      return false;
  }
  return r_prim_len > 0;
}

D3D9Batch::~D3D9Batch()
{
  d3d9_release(adjacency_vbo_);
}

bool D3D9Batch::bind_lines_adjacency()
{
  D3D9VertBuf *vbo = static_cast<D3D9VertBuf *>(verts[0]);
  if (vbo == nullptr || vbo->format.attr_len == 0 || vbo->buffer_get() == nullptr) {
    return false;
  }
  const D3D9VertexLayout &layout = vbo->layout_get();
  const uint8_t pos_type = layout.attrs[0].type;
  if (pos_type < D3DDECLTYPE_FLOAT2 || pos_type > D3DDECLTYPE_FLOAT4) {
    return false;
  }
  D3D9IndexBuf *ib = static_cast<D3D9IndexBuf *>(elem);
  D3D9IndexBuf *source_ib = ib ? ib->source_get() : nullptr;
  const uint32_t ib_version = source_ib ? source_ib->version_get() : 0;
  struct AdjacencyVert {
    float3 pos[4];
    float end;
  };
  if (adjacency_vbo_ == nullptr || adjacency_vert_ != vbo ||
      adjacency_vert_version_ != vbo->version_get() || adjacency_ib_ != source_ib ||
      adjacency_ib_version_ != ib_version)
  {
    const Span<uint32_t> indices = source_ib ? source_ib->host_indices_get() : Span<uint32_t>();
    const uint prim_len = (ib ? uint(indices.size()) : vbo->vertex_len) / 4;
    if (prim_len == 0) {
      return false;
    }
    const uint size = prim_len * 2 * sizeof(AdjacencyVert);
    IDirect3DDevice9 *device = D3D9Context::get()->device;
    if (adjacency_vbo_ == nullptr || adjacency_size_ < size) {
      d3d9_release(adjacency_vbo_);
      adjacency_size_ = 0;
      if (!D3D9_CHECK(device->CreateVertexBuffer(
              size, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &adjacency_vbo_, nullptr)))
      {
        return false;
      }
      adjacency_size_ = size;
    }
    void *src, *dst;
    if (FAILED(vbo->buffer_get()->Lock(0, 0, &src, D3DLOCK_READONLY))) {
      return false;
    }
    if (FAILED(adjacency_vbo_->Lock(0, size, &dst, 0))) {
      vbo->buffer_get()->Unlock();
      return false;
    }
    AdjacencyVert *verts_dst = static_cast<AdjacencyVert *>(dst);
    for (uint prim = 0; prim < prim_len; prim++) {
      AdjacencyVert vert = {};
      for (int i = 0; i < 4; i++) {
        const uint index = ib ? indices[prim * 4 + i] : prim * 4 + i;
        if (index < vbo->vertex_len) {
          const float *p = reinterpret_cast<const float *>(static_cast<const uint8_t *>(src) +
                                                           index * layout.stride +
                                                           layout.attrs[0].offset);
          vert.pos[i] = float3(p[0], p[1], pos_type == D3DDECLTYPE_FLOAT2 ? 0.0f : p[2]);
        }
      }
      vert.end = 0.0f;
      verts_dst[prim * 2] = vert;
      vert.end = 1.0f;
      verts_dst[prim * 2 + 1] = vert;
    }
    adjacency_vbo_->Unlock();
    vbo->buffer_get()->Unlock();
    adjacency_vert_ = vbo;
    adjacency_vert_version_ = vbo->version_get();
    adjacency_ib_ = source_ib;
    adjacency_ib_version_ = ib_version;
  }

  D3D9Context *ctx = D3D9Context::get();
  D3D9VertexDeclKey key;
  for (int i = 0; i < 4; i++) {
    key.elements.append({0,
                         WORD(i * sizeof(float3)),
                         D3DDECLTYPE_FLOAT3,
                         D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD,
                         BYTE(i)});
  }
  key.elements.append({0,
                       WORD(offsetof(AdjacencyVert, end)),
                       D3DDECLTYPE_FLOAT1,
                       D3DDECLMETHOD_DEFAULT,
                       D3DDECLUSAGE_TEXCOORD,
                       4});
  IDirect3DVertexDeclaration9 *decl = ctx->vertex_declaration_get(key);
  if (decl == nullptr) {
    return false;
  }
  ctx->device->SetVertexDeclaration(decl);
  ctx->device->SetStreamSource(0, adjacency_vbo_, 0, sizeof(AdjacencyVert));
  return true;
}

bool D3D9Batch::bind_vertex_streams(D3D9Shader *shader, const DeindexRange *deindex)
{
  D3D9Context *ctx = D3D9Context::get();
  D3D9Backend *backend = D3D9Backend::get();
  IDirect3DDevice9 *device = ctx->device;
  const ShaderInterface *interface = shader->interface;
  const uint16_t inputs = shader->vertex_inputs_get();

  D3D9VertexDeclKey key;
  uint16_t bound = 0;
  int stream = 0;
  for (int v = 0; v < GPU_BATCH_VBO_MAX_LEN; v++) {
    D3D9VertBuf *vbo = static_cast<D3D9VertBuf *>(verts[v]);
    if (vbo == nullptr) {
      continue;
    }
    IDirect3DVertexBuffer9 *buffer = deindex ? vbo->deindexed_get(
                                                   *deindex->ib, deindex->start, deindex->count) :
                                               vbo->buffer_get();
    if (buffer == nullptr) {
      continue;
    }
    const D3D9VertexLayout &layout = vbo->layout_get();
    bool used = false;
    for (uint a_idx = 0; a_idx < vbo->format.attr_len; a_idx++) {
      const GPUVertAttr &a = vbo->format.attrs[a_idx];
      for (uint n_idx = 0; n_idx < a.name_len; n_idx++) {
        const char *name = GPU_vertformat_attr_name_get(&vbo->format, &a, n_idx);
        const ShaderInput *input = interface->attr_get(name);
        if (input == nullptr || input->location < 0) {
          continue;
        }
        const int loc = input->location;
        if (!(inputs & (1 << loc)) || (bound & (1 << loc))) {
          continue;
        }
        key.elements.append({WORD(stream),
                             WORD(layout.attrs[a_idx].offset),
                             BYTE(layout.attrs[a_idx].type),
                             D3DDECLMETHOD_DEFAULT,
                             D3DDECLUSAGE_TEXCOORD,
                             BYTE(loc)});
        bound |= 1 << loc;
        used = true;
      }
    }
    if (used) {
      device->SetStreamSource(stream, buffer, 0, layout.stride);
      stream++;
    }
  }

  for (const D3D9Shader::VertexPull &pull : shader->vertex_pulls()) {
    /* Vertices read from a storage buffer: bind it as a vertex stream. */
    D3D9StorageBuf *ssbo = (pull.slot >= 0 && pull.slot < D3D9_MAX_SSBO_SLOTS) ?
                               ctx->bound_ssbos[pull.slot] :
                               nullptr;
    IDirect3DVertexBuffer9 *buffer = ssbo ? ssbo->vertex_buffer_get() : nullptr;
    if (buffer == nullptr || pull.stride <= 0) {
      return false;
    }
    int offset = 0;
    for (int column = 0; column < int(pull.types.size()); column++) {
      const bool is_color = pull.types[column] == 'c';
      const int loc = pull.location + column;
      if ((inputs & (1 << loc)) && !(bound & (1 << loc))) {
        key.elements.append({WORD(stream),
                             WORD(offset),
                             BYTE(is_color ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_FLOAT4),
                             D3DDECLMETHOD_DEFAULT,
                             D3DDECLUSAGE_TEXCOORD,
                             BYTE(loc)});
        bound |= 1 << loc;
      }
      offset += is_color ? 4 : 16;
    }
    device->SetStreamSource(stream, buffer, 0, UINT(pull.stride));
    stream++;
  }

  if (shader->uses_vertex_id()) {
    key.elements.append({WORD(stream),
                         0,
                         D3DDECLTYPE_FLOAT1,
                         D3DDECLMETHOD_DEFAULT,
                         D3DDECLUSAGE_TEXCOORD,
                         BYTE(D3D9_VERTEX_ID_ATTR)});
    device->SetStreamSource(stream, backend->vertex_id_buffer(), 0, sizeof(float));
    stream++;
  }

  /* Inputs without data read a constant white (OpenGL reads a constant attribute value). */
  const uint16_t missing = inputs & ~bound & ~(1 << D3D9_VERTEX_ID_ATTR);
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
    device->SetStreamSource(stream, backend->default_attr_buffer(), 0, sizeof(D3DCOLOR));
    stream++;
  }

  if (key.elements.is_empty()) {
    return false;
  }
  IDirect3DVertexDeclaration9 *decl = ctx->vertex_declaration_get(key);
  if (decl == nullptr) {
    return false;
  }
  device->SetVertexDeclaration(decl);
  return true;
}

void D3D9Batch::draw(int v_first, int v_count, int i_first, int i_count)
{
  D3D9Context *ctx = D3D9Context::get();
  D3D9Shader *shader = static_cast<D3D9Shader *>(ctx->shader);
  if (v_count <= 0 || i_count <= 0) {
    return;
  }
  if (!ctx->draw_setup()) {
    return;
  }
  static const bool trace = getenv("BLENDER_D3D9_TRACE") != nullptr;
  D3DPRIMITIVETYPE type;
  uint prim_len;
  /* Each primitive of 4 indices is a line of 2 vertices of the adjacency buffer. */
  const bool lines_adjacency = prim_type == GPU_PRIM_LINES_ADJ && shader->lines_adjacency();
  if (prim_type == GPU_PRIM_POINTS && shader->points_as_line_strip()) {
    /* The vertex count is the line count (lines from each point to the next). */
    type = D3DPT_LINESTRIP;
    prim_len = uint(v_count);
  }
  else if (lines_adjacency) {
    type = D3DPT_LINELIST;
    prim_len = uint(v_count / 4);
    /* The adjacency buffer holds the primitives of the whole index buffer. */
    const int index_start = elem ? int(static_cast<D3D9IndexBuf *>(elem)->draw_start_get()) : 0;
    v_first = ((index_start + v_first) / 4) * 2;
  }
  else if (!d3d9_primitive(prim_type, v_count, type, prim_len)) {
    if (trace) {
      printf("  DROP: primitive %d\n", int(prim_type));
    }
    return;
  }
  D3D9IndexBuf *ib = lines_adjacency ? nullptr : static_cast<D3D9IndexBuf *>(elem);
  /* Indexed point lists show only some of the points on Radeon 9x00 drivers: draw copies of the
   * vertices in index order instead (cached by the vertex buffers). */
  DeindexRange deindex_range;
  const DeindexRange *deindex = nullptr;
  if (ib && type == D3DPT_POINTLIST && shader->vertex_pulls().is_empty() &&
      !shader->uses_vertex_id())
  {
    deindex_range = {ib, ib->draw_start_get() + uint(v_first), uint(v_count)};
    deindex = &deindex_range;
    ib = nullptr;
    v_first = 0;
  }
  if (lines_adjacency ? (prim_len == 0 || !bind_lines_adjacency()) :
                        !bind_vertex_streams(shader, deindex))
  {
    if (trace) {
      printf("  DROP: vertex streams\n");
    }
    return;
  }
  ctx->point_sprite_set(type == D3DPT_POINTLIST && shader->point_sprite());
  ctx->flat_color_set(shader->flat_color());
  if (trace) {
    printf("  issue type=%d prims=%u v_first=%d inst=%d+%d ib=%p\n",
           int(type),
           prim_len,
           v_first,
           i_first,
           i_count,
           static_cast<void *>(elem));
  }

  IDirect3DDevice9 *device = ctx->device;
  IDirect3DIndexBuffer9 *ibo = ib ? ib->buffer_get() : nullptr;
  if (ib && ibo == nullptr) {
    return;
  }

  /* Vertex range addressed by the indices. */
  uint vert_range = D3D9_VERTEX_ID_LEN;
  if (verts[0]) {
    vert_range = verts[0]->vertex_len;
  }

  const bool close_loop = prim_type == GPU_PRIM_LINE_LOOP && v_count > 2 && ib == nullptr;

  const bool select_output = shader->select_output();
  if (select_output && !ctx->select_draw_begin()) {
    return;
  }

  const double gpu_start_ms = d3d9_gpu_time_enabled() ? d3d9_gpu_time_begin(ctx->device) : 0.0;
  const double draw_start_ms = d3d9_time_ms();
  const bool per_instance_data = shader->has_instance_data();
  for (int instance = i_first; instance < i_first + i_count; instance++) {
    if (instance != 0 || i_count > 1) {
      ctx->instance_id_set(instance);
    }
    if (per_instance_data) {
      shader->instance_data_set(instance);
      shader->constants_upload(device);
    }
    if (ib) {
      device->SetIndices(ibo);
      HRESULT hr = device->DrawIndexedPrimitive(
          type, INT(ib->index_base_get()), 0, vert_range, ib->draw_start_get() + v_first, prim_len);
      if (trace && FAILED(hr)) {
        printf("  FAILED DrawIndexedPrimitive %08lx\n", hr);
      }
    }
    else {
      HRESULT hr = device->DrawPrimitive(type, v_first, prim_len);
      if (trace && FAILED(hr)) {
        printf("  FAILED DrawPrimitive %08lx\n", hr);
      }
      if (close_loop) {
        IDirect3DIndexBuffer9 *loop_ibo = D3D9Backend::get()->line_loop_index_buffer();
        if (loop_ibo) {
          device->SetIndices(loop_ibo);
          device->DrawIndexedPrimitive(
              D3DPT_LINELIST, v_first, 0, v_count, (v_count - 1) * 2, 1);
        }
      }
    }
  }
  if (i_count > 1 || i_first != 0) {
    ctx->instance_id_set(0);
  }
  d3d9_frame_stats.draw_ms += d3d9_time_ms() - draw_start_ms;
  if (d3d9_gpu_time_enabled()) {
    const double before_ms = d3d9_time_ms();
    d3d9_gpu_time_end(ctx->device, shader->name_get().c_str(), gpu_start_ms);
    if (before_ms - gpu_start_ms > 1.0 || d3d9_time_ms() - gpu_start_ms > 1.0) {
      const Span<float4> vs = shader->vs_constants_get();
      printf("d3d9: slow draw %s %.2f ms, %d vertices, %d instances, vs c2..c8:",
             shader->name_get().c_str(),
             d3d9_time_ms() - gpu_start_ms,
             v_count,
             i_count);
      for (int c = 2; c < min_ii(vs.size(), 9); c++) {
        printf(" (%g %g %g %g)", vs[c].x, vs[c].y, vs[c].z, vs[c].w);
      }
      printf("\n");
    }
  }
  if (select_output) {
    ctx->select_draw_end();
  }
}

void D3D9Batch::draw_indirect(StorageBuf *indirect_buf, intptr_t offset)
{
  /* Indirect arguments live in host memory. */
  const D3D9StorageBuf *buf = static_cast<D3D9StorageBuf *>(indirect_buf);
  const uint8_t *data = buf->data_get();
  if (data == nullptr) {
    return;
  }
  if (elem) {
    /* DrawElementsIndirectCommand: count, instance_count, first_index, base_vertex, base_inst.
     * The first index is in the source buffer (sub-range start included), `draw` adds the
     * sub-range start. */
    const uint32_t *cmd = reinterpret_cast<const uint32_t *>(data + offset);
    const int first = int(cmd[2]) - int(static_cast<D3D9IndexBuf *>(elem)->draw_start_get());
    draw(first, int(cmd[0]), int(cmd[4]), int(cmd[1]));
  }
  else {
    /* DrawArraysIndirectCommand: count, instance_count, first, base_instance. */
    const uint32_t *cmd = reinterpret_cast<const uint32_t *>(data + offset);
    draw(int(cmd[2]), int(cmd[0]), int(cmd[3]), int(cmd[1]));
  }
}

void D3D9Batch::multi_draw_indirect(StorageBuf *indirect_buf,
                                    int count,
                                    intptr_t offset,
                                    intptr_t stride)
{
  for (int i = 0; i < count; i++) {
    draw_indirect(indirect_buf, offset + i * stride);
  }
}

}  // namespace blender::gpu
