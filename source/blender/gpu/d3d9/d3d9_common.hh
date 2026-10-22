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

/** Report a failed D3D call (once per call site), returns true on success. */
bool d3d9_check(HRESULT hr, const char *call, const char *file, int line);
#define D3D9_CHECK(call) blender::gpu::d3d9_check((call), #call, __FILE__, __LINE__)

}  // namespace blender::gpu
