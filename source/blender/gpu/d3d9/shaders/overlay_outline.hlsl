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
  /* Neighbors: right, left, up, down (interpolated, the pixel shader is the expensive part). */
  float2 uv_right : TEXCOORD1;
  float2 uv_left : TEXCOORD2;
  float2 uv_up : TEXCOORD3;
  float2 uv_down : TEXCOORD4;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float2 pos = drw_fullscreen_pos(v.vertex_id);
  o.position = gpu_position(float4(pos, 1.0, 1.0));
  o.uv = pos * 0.5 + 0.5;
  float2 ofs = uniform_buf__97.zw;
  o.uv_right = o.uv + float2(ofs.x, 0.0);
  o.uv_left = o.uv - float2(ofs.x, 0.0);
  o.uv_up = o.uv + float2(0.0, ofs.y);
  o.uv_down = o.uv - float2(0.0, ofs.y);
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

FragOut frag(VertOut v)
{
  FragOut o;
  float ref = tex2D(outline_id_tx, v.uv).r;
  float4 ids = float4(tex2D(outline_id_tx, v.uv_right).r,
                      tex2D(outline_id_tx, v.uv_left).r,
                      tex2D(outline_id_tx, v.uv_up).r,
                      tex2D(outline_id_tx, v.uv_down).r);
  /* The ids are integers: a difference saturates to 1. */
  float4 edges = saturate(abs(ids - ref));
  /* X-ray wires: only the horizontal neighbors. */
  edges.yw *= 1.0 - is_xray_wires;
  float edge_count = dot(edges, 1.0);
  clip(min(edge_count - 0.5, 3.5 - edge_count));

  /* Make the outline bleed on the background: take a neighbor id (the largest). */
  float ref_col = ref > 0.0 ? ref : max(max(ids.x, ids.y), max(ids.z, ids.w));
  /* Color id: transform 0, selected 1, active 3. */
  float color_id = floor(ref_col * (1.0 / 16384.0));
  float4 color = lerp(uniform_buf__2[4], uniform_buf__2[1], saturate(color_id));
  color = lerp(color, uniform_buf__2[0], saturate(color_id - 2.0));
  o.color = color * (254.0 / 255.0);
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
  return o;
}
#endif
