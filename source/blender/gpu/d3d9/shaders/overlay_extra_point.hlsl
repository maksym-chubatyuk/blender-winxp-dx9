/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Object centers (`overlay_extra_point`) and loose points of mesh objects
 * (`overlay_extra_loose_point`, define LOOSE). The points are read from `data_buf` (VertexData:
 * position, color).
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

// d3d9-resource-id 11 1
// d3d9-vertex-pull 0 32
// d3d9-point-sprite
float4 view_buf[12];
/* Theme sizes: pixel, object_center; light_circle_shadow, vert. */
float4 uniform_buf__94[2];
/* Theme colors: outline. */
float4 uniform_buf__39;
/* Theme colors: edit_mesh_middle. */
float4 uniform_buf__46;

struct VertIn {
  float4 pos : TEXCOORD0;
  float4 color : TEXCOORD1;
};

struct VertOut {
  float4 position : POSITION;
  float point_size : PSIZE;
  float4 fill_color : COLOR0;
  /* Point radii (outer, outer AA, inner, inner AA), divided by the point size. */
  float4 radii : COLOR1;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos.xyz);
  float4 clip_pos = drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0)));
  o.position = gpu_position(clip_pos);
  o.fill_color = saturate(v.color);
#ifdef LOOSE
  o.point_size = uniform_buf__94[1].y * 2.0;
  o.radii = float4(0.0, 0.0, 0.0, 0.0);
#else
  float size = uniform_buf__94[0].y;
  float outline_width = uniform_buf__94[0].x;
  o.point_size = size;
  float radius = 0.5 * size;
  o.radii = saturate(float4(radius, radius - 1.0, radius - outline_width,
                            radius - outline_width - 1.0) /
                     size);
#endif
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

FragOut frag(VertOut v, float2 point_coord : TEXCOORD0)
{
  FragOut o;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
#ifdef LOOSE
  float2 centered = abs(point_coord - 0.5);
  float dist = max(centered.x, centered.y);
  float fac = dist * dist * 4.0;
  /* Non linear blend. */
  float4 col = lerp(sqrt(uniform_buf__46), sqrt(v.fill_color), 0.45 + fac * 0.65);
  o.color = col * col;
#else
  /* `overlay_point_varying_color_varying_outline_aa_frag.glsl` */
  float dist = length(point_coord - 0.5);
  float4 radii = v.radii;
  float mid_stroke = 0.5 * (radii.y + radii.z);
  float4 outline = float4(uniform_buf__39.rgb,
                          lerp(uniform_buf__39.a, 0.0, smoothstep(radii.y, radii.x, dist)));
  float4 fill = lerp(v.fill_color, uniform_buf__39, smoothstep(radii.w, radii.z, dist));
  o.color = dist > mid_stroke ? outline : fill;
#endif
  return o;
}
