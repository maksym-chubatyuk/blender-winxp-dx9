/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Icons and image rectangles drawn from 4 procedural vertices.
 * Defines: ICON (corner masking for the text overlay), RECT_COLOR (`gpu_shader_2D_image_rect_color`),
 * MULTI (`gpu_shader_icon_multi`, rectangles from the `multi_icon_data` uniform buffer).
 */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
#ifdef MULTI
float4 multi_icon_data[48];
#else
float4 rect_icon;
float4 rect_geom;
#endif
#ifdef RECT_COLOR
float4 color;
#else
float4 finalColor;
#endif
#ifdef ICON
float text_width;
#endif
sampler2D image : register(s0);

struct VertIn {
#ifdef MULTI
  float2 pos : TEXCOORD0;
#endif
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
  float2 mask_coord : TEXCOORD1;
  float4 color : TEXCOORD2;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float2 uv, co;
#ifdef MULTI
  int base = int(gpu_InstanceID) * 3;
  float4 rect = multi_icon_data[base];
  float4 tex = multi_icon_data[base + 1];
  o.color = multi_icon_data[base + 2];
  float2 sel = v.pos;
  co = float2(sel.x > 0.5 ? rect.y : rect.x, sel.y > 0.5 ? rect.w : rect.z);
  uv = float2(sel.x > 0.5 ? tex.y : tex.x, sel.y > 0.5 ? tex.w : tex.z);
  o.mask_coord = float2(0.0, 0.0);
  /* Already in normalized device coordinates. */
  o.position = gpu_position(float4(co, 0.0, 1.0));
#else
  /* Vertex 0: (x, w), 1: (x, y), 2: (z, w), 3: (z, y). */
  bool right = v.vertex_id > 1.5;
  bool bottom = frac(v.vertex_id * 0.5) > 0.25;
  co = float2(right ? rect_geom.z : rect_geom.x, bottom ? rect_geom.y : rect_geom.w);
  uv = float2(right ? rect_icon.z : rect_icon.x, bottom ? rect_icon.y : rect_icon.w);
  o.mask_coord = float2(right ? 0.0 : -1.0, bottom ? 0.0 : 1.0);
#  ifdef RECT_COLOR
  o.color = color;
#  else
  o.color = finalColor;
#  endif
  o.position = gpu_position(mul(ModelViewProjectionMatrix, float4(co, 0.0, 1.0)));
#endif
  o.uv = uv;
  return o;
}

float4 frag(VertOut v) : COLOR0
{
#ifdef RECT_COLOR
  float4 c = tex2D(image, v.uv) * v.color;
#else
  /* The icon sampler state has the -0.5 LOD bias. */
  float4 c = tex2D(image, v.uv) * v.color;
#endif
#ifdef ICON
  const float circle_radius_outer = 0.1;
  const float circle_radius_inner = 0.075;
  const float mask_transparency = 0.25;
  float2 circle_center = float2(circle_radius_outer - text_width, 0.5);
  float radius = length(v.mask_coord - circle_center);
  float mask = smoothstep(circle_radius_inner, circle_radius_outer, radius);
  bool lower_half = v.mask_coord.y < circle_center.y;
  bool right_half = v.mask_coord.x > circle_center.x;
  if (right_half && v.mask_coord.y < circle_center.y + circle_radius_outer) {
    mask = smoothstep(circle_center.y + circle_radius_inner,
                      circle_center.y + circle_radius_outer,
                      v.mask_coord.y);
  }
  if (lower_half && v.mask_coord.x > circle_center.x - circle_radius_outer) {
    mask = smoothstep(circle_center.x - circle_radius_inner,
                      circle_center.x - circle_radius_outer,
                      v.mask_coord.x);
  }
  c *= max(mask_transparency, mask);
#endif
  return blender_srgb_to_framebuffer_space(c);
}
