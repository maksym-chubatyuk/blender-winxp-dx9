/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared code of the armature shaders (`overlay_armature_*`): bone instances are drawn one draw
 * per instance, their data copied to constants.
 */

#include "d3d9_common.hlsl"
#define DRW_NO_MODEL
#include "draw_common.hlsl"
#include "select_common.hlsl"

#ifdef SELECT
// d3d9-instance select_instance_id 5 4 0 instance u
float4 select_instance_id;
#endif

float4 view_buf[12];
/* size_viewport, size_viewport_inv. */
float4 uniform_buf__97;
float alpha;

#define size_viewport uniform_buf__97.xy
#define size_viewport_inv uniform_buf__97.zw

bool is_persp()
{
  return view_buf[DRW_WINMAT + 3].w == 0.0;
}

float4 view_to_clip(float3 vs_P)
{
  return drw_mat_mul(view_buf, DRW_WINMAT, float4(vs_P, 1.0));
}

float3 world_to_view(float3 ws_P)
{
  return drw_mat_mul(view_buf, DRW_VIEWMAT, float4(ws_P, 1.0)).xyz;
}

/* Pixel position of a clip space position. */
float2 clip_to_screen(float4 hs_P)
{
  return (0.5 * (hs_P.xy / hs_P.w) + 0.5) * size_viewport;
}

/* `extract_matrix_packed_data`: 4 columns of a bone matrix, the W components hold 2 colors of 4
 * bytes packed in 4 integers stored as floats. */
struct BoneMatrix {
  float3 x;
  float3 y;
  float3 z;
  float3 loc;
  float4 state_color;
  float4 bone_color;
};

float2 unpack_bytes(float value)
{
  float high = floor(value / 256.0);
  return float2(value - high * 256.0, high) / 255.0;
}

BoneMatrix bone_matrix_unpack(float4 columns[4])
{
  BoneMatrix m;
  m.x = columns[0].xyz;
  m.y = columns[1].xyz;
  m.z = columns[2].xyz;
  m.loc = columns[3].xyz;
  m.state_color = float4(unpack_bytes(columns[0].w), unpack_bytes(columns[1].w));
  m.bone_color = float4(unpack_bytes(columns[2].w), unpack_bytes(columns[3].w));
  return m;
}

float3 bone_point(BoneMatrix m, float3 p)
{
  return m.x * p.x + m.y * p.y + m.z * p.z + m.loc;
}

float bone_determinant(BoneMatrix m)
{
  return dot(cross(m.x, m.y), m.z);
}

/* Position outside of the clip volume (after `gpu_position`): discards the primitive. */
#define DISCARDED_POSITION float4(0.0, 0.0, -1.0, 1.0)

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};
