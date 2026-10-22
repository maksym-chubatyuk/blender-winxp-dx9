/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Element ids for the edit mode selection (`select_id_flat`, `select_id_uniform` with define
 * UNIFORM_ID), written to a `UINT_32` texture: on D3D9 its texels are the 4 bytes of the id (see
 * `D3D9TexelKind::PACKED_U32`), computed here as the pixel shader floats can't hold all ids.
 *
 * D3D9 difference: no retopology offset.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

// d3d9-resource-id 11 1
/* The id of a line is the one of its first vertex (`FLAT` in the GLSL). */
// d3d9-flat-color
float4 view_buf[12];
float vertex_size;
#ifdef UNIFORM_ID
float select_id;
#else
float offset;
#endif

struct VertIn {
  float3 pos : TEXCOORD0;
#ifndef UNIFORM_ID
  float index : TEXCOORD1;
#endif
};

struct VertOut {
  float4 position : POSITION;
  float point_size : PSIZE;
  /* Bytes of the id as the texel colors: B = bits 0-7, G = 8-15, R = 16-23, A = 24-31. Exact in
   * the 8 bit color interpolators. */
  float4 id_color : COLOR0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  o.position = gpu_position(drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0))));
  o.point_size = vertex_size;
#ifdef UNIFORM_ID
  float id = select_id;
#else
  float id = offset + v.index;
#endif
  float4 bytes = floor(id / float4(65536.0, 256.0, 1.0, 16777216.0));
  bytes = bytes - floor(bytes / 256.0) * 256.0;
  o.id_color = bytes / 255.0;
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  return v.id_color;
}
