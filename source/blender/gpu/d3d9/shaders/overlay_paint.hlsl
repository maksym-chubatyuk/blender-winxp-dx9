/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Paint mode overlays (`overlay_paint_*`). Defines: WEIGHT (FAKE_SHADING), FACE, POINT, WIRE.
 *
 * D3D9 differences: no weight contour lines (ps_2_0 has no derivatives).
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];
/* Theme colors: wire (0) to vert_missing_data (15). */
float4 uniform_buf__0[16];

#define TH_WIRE uniform_buf__0[0]
#define TH_VERT_UNREFERENCED uniform_buf__0[14]
#define TH_VERT_MISSING_DATA uniform_buf__0[15]

#if defined(WEIGHT)
sampler2D colorramp : register(s0);
float opacity;
#  ifdef FAKE_SHADING
float3 light_dir;
#  endif
struct VertIn {
  float weight : TEXCOORD0;
  float3 pos : TEXCOORD1;
  float3 nor : TEXCOORD2;
};
#else
struct VertIn {
  float3 pos : TEXCOORD0;
  float paint_overlay_flag : TEXCOORD1;
};
#endif

#if defined(FACE)
float4 ucolor;
#elif defined(WIRE)
float use_select;
#elif defined(POINT)
/* Theme sizes: light_circle_shadow, vert, edge, face_dot. */
float4 uniform_buf__95;
#endif

#ifdef POINT
// d3d9-point-sprite
struct VertOut {
  float4 position : POSITION;
  float point_size : PSIZE;
  float4 color : COLOR0;
};
#else
struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
#  ifdef WEIGHT
  /* Weight, alert, color factor. */
  float3 weight : TEXCOORD1;
#  endif
};
#endif

#define DISCARDED_POSITION float4(0.0, 0.0, -1.0, 1.0)

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  float4 clip_pos = drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0)));
  o.color = float4(0.0, 0.0, 0.0, 0.0);
#if defined(WEIGHT)
  /* Separate actual weight and alerts for independent interpolation. */
  o.weight.xy = max(float2(v.weight, -v.weight), 0.0);
#  ifdef FAKE_SHADING
  /* Saturate the weight to give a hint of the geometry behind the weights. */
  float3 view_normal = normalize(
      drw_mat_mul3(view_buf, DRW_VIEWMAT, drw_normal_object_to_world(v.nor)));
  o.weight.z = abs(dot(view_normal, light_dir)) * 0.9 + 0.1;
#  else
  o.weight.z = 1.0;
#  endif
#else
  bool is_select = v.paint_overlay_flag > 0.0;
  bool is_hidden = v.paint_overlay_flag < 0.0;
#  if defined(FACE)
  /* Don't draw faces that are selected. */
  clip_pos = (is_hidden || is_select) ? float4(-2.0, -2.0, -2.0, 1.0) : clip_pos;
  o.color = ucolor;
#  elif defined(WIRE)
  is_select = is_select && use_select > 0.5;
  is_hidden = is_hidden && use_select > 0.5;
  /* Offset in Z to avoid Z-fighting and render selected wires on top. */
  clip_pos.z -= is_select ? 2e-4 : 1e-4;
  clip_pos = is_hidden ? float4(-2.0, -2.0, -2.0, 1.0) : clip_pos;
  o.color = is_select ? float4(1.0, 1.0, 1.0, 1.0) : TH_WIRE;
  /* Weight paint needs a light color to contrasts with dark weights. */
  o.color = use_select > 0.5 ? o.color : float4(1.0, 1.0, 1.0, 0.3);
#  else
  clip_pos.z -= is_select ? 2e-4 : 1e-4;
  clip_pos = is_hidden ? float4(-2.0, -2.0, -2.0, 1.0) : clip_pos;
  o.color = is_select ? float4(1.0, 1.0, 1.0, 1.0) : TH_WIRE;
  o.color.a = saturate(v.paint_overlay_flag);
  o.point_size = uniform_buf__95.y * 2.0;
#  endif
#endif
  o.position = gpu_position(clip_pos);
  return o;
}

#ifdef WEIGHT
struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

FragOut frag(VertOut v)
{
  FragOut o;
  float alert = v.weight.y;
  float color_fac = v.weight.z;
  float4 weight_color = tex2D(colorramp, float2(v.weight.x, 0.5));
  /* Zero weight alert color. Nonlinear blend to reduce impact. */
  float3 color = lerp(weight_color.rgb, TH_VERT_UNREFERENCED.rgb, alert * alert);
  /* Missing vertex group alert color. Uniform in practice. */
  color = alert > 1.1 ? TH_VERT_MISSING_DATA.rgb : color;
  o.color = float4(max(0.005, color) * color_fac, opacity);
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
  return o;
}
#elif defined(POINT)
float4 frag(VertOut v, float2 point_coord : TEXCOORD0) : COLOR0
{
  float2 centered = point_coord - 0.5;
  /* Round point with jagged edges. */
  clip(0.25 - dot(centered, centered));
  return v.color;
}
#else
float4 frag(VertOut v) : COLOR0
{
  return v.color;
}
#endif
