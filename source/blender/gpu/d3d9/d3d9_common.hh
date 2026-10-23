/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Direct3D 9 backend, targeting Shader Model 2.0 hardware (Windows XP era GPUs).
 *
 * Conventions shared by all the backend files:
 * - Shaders are hand written HLSL (`d3d9/shaders`), compiled to `vs_2_0`/`ps_2_0` bytecode at build
 *   time and looked up by the name of the #ShaderCreateInfo they replace.
 * - Vertex attributes use the `TEXCOORD<n>` semantic, `n` being the attribute index of the create
 *   info. `TEXCOORD15` is reserved for the vertex index (`gl_VertexID`).
 * - Vertex shader registers `c0`-`c1` are reserved, see `d3d9_common.hlsl`.
 * - Off-screen render targets are rendered upside-down (flipped by the vertex shader epilogue) so
 *   their memory layout matches OpenGL: row 0 is the bottom row. Only the window back buffers use
 *   the native D3D orientation.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <d3d9.h>
/* COM headers define `interface` as `struct`, it is used as an identifier by Blender. */
#undef interface

#include "CLG_log.h"

namespace blender::gpu {

/** Vertex shader input usage index reserved for the vertex index. */
constexpr int D3D9_VERTEX_ID_ATTR = 15;
/** Corner of the quads of points drawn as quads (`d3d9-point-quads`), -1 or 1 per axis. */
constexpr int D3D9_POINT_CORNER_ATTR = 14;
/** Size of the dynamic vertex buffer of the immediate mode. */
constexpr uint D3D9_IMM_BUFFER_SIZE = 2 * 1024 * 1024;
/** Number of reserved vertex shader constant registers. */
constexpr int D3D9_RESERVED_VS_CONSTANTS = 2;
/** Size of the static vertex index stream. */
constexpr int D3D9_VERTEX_ID_LEN = 65536;

/** Release a COM pointer and clear it. */
template<typename T> inline void d3d9_release(T *&ptr)
{
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

/**
 * `BLENDER_D3D9_HEAPCHECK=<level>`: validate the process heap (the C runtime's and the managed
 * resources' system copies) after the operation named `where`, report the first one after which it
 * is corrupt. Level 1 checks after uploads, read-backs and frames, level 2 after every draw too
 * (very slow).
 */
void d3d9_heap_check(const char *where, int level = 1);

/** Runs #d3d9_heap_check when the scope ends (every return of a function). */
struct D3D9HeapCheckScope {
  const char *where;
  int level;
  ~D3D9HeapCheckScope()
  {
    d3d9_heap_check(where, level);
  }
};
#define D3D9_HEAP_CHECK_SCOPE(where) const D3D9HeapCheckScope d3d9_heap_check_scope_{where, 1}
#define D3D9_HEAP_CHECK_SCOPE_DRAW(where) \
  const D3D9HeapCheckScope d3d9_heap_check_scope_{where, 2}

/** Bytes of render targets and depth surfaces currently allocated (video memory). */
int64_t d3d9_render_target_bytes();

/** Report a failed D3D call (once per call site), returns true on success. */
bool d3d9_check(HRESULT hr, const char *call, const char *file, int line);
#define D3D9_CHECK(call) blender::gpu::d3d9_check((call), #call, __FILE__, __LINE__)

}  // namespace blender::gpu
