/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Anti-aliased round points of uniform size and color (the dots of the node editor grid), see
 * `gpu_shader_2D_point_uniform_size_aa_vert.glsl`, `gpu_shader_point_uniform_color_aa_frag.glsl`.
 */

#include "d3d9_common.hlsl"

// d3d9-point-sprite
float4x4 ModelViewProjectionMatrix;
float4 color;
float size;

struct VertIn {
  float2 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  float point_size : PSIZE;
  /* Outer and inner radius, divided by the point size (sprites replace the texture
   * coordinates). */
  float4 radii : COLOR0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  o.position = gpu_position(mul(ModelViewProjectionMatrix, float4(v.pos, 0.0, 1.0)));
  o.point_size = size;
  float radius = 0.5 * size;
  o.radii = saturate(float4(radius, radius - 1.0, 0.0, 0.0) / size);
  return o;
}

float4 frag(VertOut v, float2 point_coord : TEXCOORD0) : COLOR0
{
  float dist = length(point_coord - 0.5);
  float4 frag_color = blender_srgb_to_framebuffer_space(color);
  frag_color.a = lerp(color.a, 0.0, smoothstep(v.radii.y, v.radii.x, dist));
  clip(frag_color.a - 1e-6);
  return frag_color;
}
