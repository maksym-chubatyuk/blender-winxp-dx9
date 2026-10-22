/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Extra lines (`overlay_extra_wire`): relationship lines, camera paths... read from `data_buf`
 * (VertexData: position, color). With OBJECT_WIRE (`overlay_extra_wire_object`) the lines are
 * vertex attributes and the color is packed in the object matrix.
 *
 * D3D9 difference: no stipple (needs the line start point, a flat varying), lines are solid.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];

#ifdef OBJECT_WIRE
struct VertIn {
  float3 pos : TEXCOORD0;
};
#else
// d3d9-vertex-pull 0 32
/* Theme colors: camera_path. */
float4 uniform_buf__10;
float colorid;

struct VertIn {
  float4 pos : TEXCOORD0;
  float4 color : TEXCOORD1;
};
#endif

struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos.xyz);
  float4 clip_pos = drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0)));
  o.position = gpu_position(clip_pos);
#ifdef OBJECT_WIRE
  /* Extract data packed inside the unused float4x4 members. */
  o.color = float4(drw_model[0].w, drw_model[1].w, drw_model[2].w, drw_model[3].w);
#else
  o.color = colorid != 0.0 ? uniform_buf__10 : v.color;
#endif
  o.color = saturate(float4(o.color.rgb, 1.0));
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

FragOut frag(VertOut v)
{
  FragOut o;
  o.color = v.color;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
  return o;
}
