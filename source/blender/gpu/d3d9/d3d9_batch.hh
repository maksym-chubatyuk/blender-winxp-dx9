/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "GPU_batch.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

class D3D9IndexBuf;
class D3D9Shader;

/** D3D9 primitive type and primitive count for a vertex count, returns false when unsupported. */
bool d3d9_primitive(GPUPrimType prim_type,
                    uint vert_len,
                    D3DPRIMITIVETYPE &r_type,
                    uint &r_prim_len);

class D3D9Batch : public Batch {
 public:
  ~D3D9Batch() override;

  void draw(int v_first, int v_count, int i_first, int i_count) override;
  void draw_indirect(StorageBuf *indirect_buf, intptr_t offset) override;
  void multi_draw_indirect(StorageBuf *indirect_buf,
                           int count,
                           intptr_t offset,
                           intptr_t stride) override;

 private:
  /** Indices drawn from de-indexed copies of the vertex buffers. */
  struct DeindexRange {
    D3D9IndexBuf *ib;
    uint start;
    uint count;
  };

  /** Bind the vertex streams and declaration for the bound shader. */
  bool bind_vertex_streams(D3D9Shader *shader, const DeindexRange *deindex = nullptr);
  /**
   * Bind the lines with adjacency of the batch as a line list: 2 vertices per primitive holding
   * the 4 positions of the primitive (TEXCOORD0 to 3) and the line end (TEXCOORD4, 0 or 1).
   */
  bool bind_lines_adjacency();

  IDirect3DVertexBuffer9 *adjacency_vbo_ = nullptr;
  uint adjacency_size_ = 0;
  const void *adjacency_vert_ = nullptr;
  uint32_t adjacency_vert_version_ = 0;
  const void *adjacency_ib_ = nullptr;
  uint32_t adjacency_ib_version_ = 0;
};

}  // namespace blender::gpu
