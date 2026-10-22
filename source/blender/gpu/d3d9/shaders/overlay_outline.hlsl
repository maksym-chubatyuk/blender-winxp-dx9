/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Selection outlines. Defines: PREPASS (`overlay_outline_prepass_mesh`: object ids), DETECT
 * (`overlay_outline_detect`: edges of the ids).
 *
 * D3D9 differences: the ids are stored in a float target (exact for 16 bit values). Without depth
 * sampling, outlines are never dimmed when occluded. No line anti-aliasing data.
 */

#include "d3d9_common.hlsl"

#ifdef PREPASS
#  include "draw_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];
/* ObjectInfos.flag of the instance. */
// d3d9-instance ob_flag 9 80 60 res u
float4 ob_flag;
/* Resource id of the instance. */
// d3d9-instance ob_resource_id 11 4 0 instance u
float4 ob_resource_id;
float is_transform;

struct VertIn {
  float3 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  float ob_id : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  float4 clip_pos = drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0)));
  /* Small bias to always be on top of the geometry. */
  clip_pos.z -= 1e-3;
  o.position = gpu_position(clip_pos);
  /* Color id: transform 0, active 3 (OBJECT_ACTIVE flag), selected 1. */
  float is_active = fmod(floor(ob_flag.x / 8.0), 2.0);
  float color_id = is_transform > 0.5 ? 0.0 : (is_active > 0.5 ? 3.0 : 1.0);
  /* `outline_id_pack`: 2 bits of color, 14 bits of object id. 0 is the background. */
  o.ob_id = color_id * 16384.0 + fmod(ob_resource_id.x + 1.0, 16384.0);
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  return v.ob_id.xxxx;
}

#else /* DETECT */
#  define DRW_NO_MODEL
#  include "draw_common.hlsl"

/* Theme colors: object_select (3), active_object (2), transform (6). */
float4 uniform_buf__2[5];
/* size_viewport, size_viewport_inv. */
float4 uniform_buf__97;
float is_xray_wires;
sampler2D outline_id_tx : register(s0);

struct VertIn {
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float2 pos = drw_fullscreen_pos(v.vertex_id);
  o.position = gpu_position(float4(pos, 1.0, 1.0));
  o.uv = pos * 0.5 + 0.5;
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

FragOut frag(VertOut v)
{
  FragOut o;
  float2 ofs = uniform_buf__97.zw;
  float ref = tex2D(outline_id_tx, v.uv).r;
  float4 ids = float4(tex2D(outline_id_tx, v.uv + float2(ofs.x, 0.0)).r,
                      tex2D(outline_id_tx, v.uv - float2(ofs.x, 0.0)).r,
                      tex2D(outline_id_tx, v.uv + float2(0.0, ofs.y)).r,
                      tex2D(outline_id_tx, v.uv - float2(0.0, ofs.y)).r);
  float4 edges = (ids != ref) ? 1.0 : 0.0;
  if (is_xray_wires > 0.5) {
    edges.yw = 0.0;
  }
  /* Make the outline bleed on the background: take the first neighbor id. */
  float ref_col = ref;
  ref_col = (ref_col == 0.0) ? ids.x : ref_col;
  ref_col = (ref_col == 0.0) ? ids.y : ref_col;
  ref_col = (ref_col == 0.0) ? ids.z : ref_col;
  ref_col = (ref_col == 0.0) ? ids.w : ref_col;

  float edge_count = dot(edges, 1.0);
  clip((edge_count > 0.5 && edge_count < 3.5) ? 1.0 : -1.0);

  float color_id = floor(ref_col / 16384.0);
  float4 color = (color_id > 0.5 && color_id < 1.5) ? uniform_buf__2[1] :
                 (color_id > 2.5) ? uniform_buf__2[0] : uniform_buf__2[4];
  o.color = (ref_col == 0.0) ? 0.0 : color * (254.0 / 255.0);
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
  return o;
}
#endif
