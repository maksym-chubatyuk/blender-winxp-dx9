/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Node editor sockets, see `gpu_shader_2D_node_socket_vert.glsl` and `_frag.glsl`.
 * Defines: INSTANCED (parameters of up to 32 sockets, indexed by the instance).
 *
 * SM2.0 differences: the shape selects a distance function in the vertex shader (rotation, half
 * size, circle or square) and the thresholds are computed there too, the fragment shader has no
 * branches. The volume grid and list shapes are drawn as rectangles.
 */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
#ifdef INSTANCED
float4 parameters[128];
#  define PARAM(i) parameters[base + i]
#else
float4 parameters[4];
#  define PARAM(i) parameters[i]
#endif

struct VertIn {
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float4 uv_extrusion : TEXCOORD0;
  float4 color : TEXCOORD1;
  float4 outline_color : TEXCOORD2;
  /* Half size of the square shapes, 45 degree rotation, circle. */
  float4 shape : TEXCOORD3;
  /* Thresholds: alpha (inner, outer), outline (inner, outer). */
  float4 thresholds : TEXCOORD4;
  /* Dot thresholds (inner, outer). */
  float2 dot_thresholds : TEXCOORD5;
};

/* Upper and lower limit for an anti-aliased cutoff of the squared distance. */
float2 calculate_thresholds(float threshold, float aa_size)
{
  float inner = threshold - 0.5 * aa_size;
  float outer = threshold + 0.5 * aa_size;
  return float2(inner * abs(inner), outer * abs(outer));
}

VertOut vert(VertIn v)
{
#ifdef INSTANCED
  int base = int(gpu_InstanceID) * 4;
#endif
  float4 rect = PARAM(0);
  float4 p3 = PARAM(3); /* outline thickness, outline offset, shape, aspect */

  /* Scale the original rectangle to accommodate the diagonal of the diamond shape. */
  float2 original_size = rect.yw - rect.xz;
  float offset = 0.125 * min(original_size.x, original_size.y) + p3.y * p3.x;
  float right = v.vertex_id > 1.5;
  float top = frac(v.vertex_id * 0.5) > 0.25;
  float2 pos = float2(lerp(rect.x - offset, rect.y + offset, right),
                      lerp(rect.z - offset, rect.w + offset, top));

  float4 clip_pos = mul(ModelViewProjectionMatrix, float4(pos, 0.0, 1.0));

  float2 rect_size = original_size + 2.0 * p3.y;
  float min_size = min(rect_size.x, rect_size.y);
  float2 uv = (pos - (rect.xz + rect.yw) * 0.5) / min_size;

  /* Extrusion of the coordinates to draw the middle part of multi sockets. */
  float ratio = rect_size.x / rect_size.y;
  float2 extrusion = (ratio > 1.0) ? float2((ratio - 1.0) * 0.5, 0.0) :
                                     float2(0.0, (1.0 / ratio - 1.0) * 0.5);

  float outline_thickness = p3.x / min_size;
  float aa_size = p3.w / min_size;

  /* `eNodeSocketDisplayShape`: circle, square, diamond, then the same with a dot, line, volume
   * grid, list. */
  float shape = p3.z;
  float with_dot = (shape > 2.5) * (shape < 5.5);
  float base_shape = shape - 3.0 * with_dot;
  float is_circle = base_shape < 0.5;
  float is_diamond = abs(base_shape - 2.0) < 0.5;
  /* `0.5 / sqrt(2 / pi) * sqrt(0.5)`. */
  const float square_radius = 0.443113;
  float2 half_size = float2(square_radius, square_radius);
  half_size = lerp(half_size, float2(0.75, 1.4) * square_radius, abs(shape - 6.0) < 0.5);
  /* Volume grid and list: one rectangle the size of their outer corners. */
  float2 grid_size = max(float2(0.25, 0.25), 0.5 * extrusion + float2(0.0, square_radius * 0.56)) +
                     0.75 * outline_thickness;
  half_size = lerp(half_size, grid_size, shape > 6.5);

  float alpha_threshold = 0.5 * is_circle;
  float dot_threshold = lerp(-1.0, outline_thickness, with_dot);

  VertOut o;
  o.position = gpu_position(clip_pos);
  o.uv_extrusion = float4(uv, extrusion);
  o.color = PARAM(1);
  o.outline_color = PARAM(2);
  o.shape = float4(half_size, is_diamond, is_circle);
  o.thresholds = float4(calculate_thresholds(alpha_threshold, aa_size),
                        calculate_thresholds(alpha_threshold - outline_thickness, aa_size));
  o.dot_thresholds = calculate_thresholds(dot_threshold, aa_size);
  return o;
}

/* `smoothstep` that also works with the edges in decreasing order. */
float ramp(float edge0, float edge1, float x)
{
  float t = saturate((x - edge0) / (edge1 - edge0));
  return t * t * (3.0 - 2.0 * t);
}

float4 frag(VertOut v) : COLOR0
{
  float2 co = max(abs(v.uv_extrusion.xy) - v.uv_extrusion.zw, 0.0);
  float co_squared = dot(co, co);

  /* Squared distance field of a square, optionally rotated by 45 degrees. */
  float2 rotated = abs(float2(co.x - co.y, co.x + co.y)) * 0.707107;
  float2 extruded = lerp(co, rotated, v.shape.z) - v.shape.xy;
  float2 clamped = max(extruded, 0.0);
  float interior = min(max(extruded.x, extruded.y), 0.0);
  float square_distance = dot(clamped, clamped) - interior * interior;

  float distance_squared = lerp(square_distance, co_squared, v.shape.w);

  float alpha_mask = ramp(v.thresholds.y, v.thresholds.x, distance_squared);
  float dot_mask = ramp(v.dot_thresholds.y, v.dot_thresholds.x, co_squared);
  float outline_mask = ramp(v.thresholds.z, v.thresholds.w, distance_squared) + dot_mask;

  float4 color = lerp(v.color, v.outline_color, outline_mask);
  color.a *= alpha_mask;
  return color;
}
