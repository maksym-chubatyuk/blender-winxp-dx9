/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Node editor links (cubic Bezier thick lines), see `gpu_shader_2D_nodelink.bsl.hh`.
 * The link of the drawn instance is copied from the `NodeLinkData` storage buffer.
 */

#include "d3d9_common.hlsl"

/* `NodeLinkData`: start color, end color, P0 P1, P2 P3, color ids, (dash length, dash factor,
 * dash alpha, dim factor), (thickness, aspect, do_arrow, do_muted), (has_back_link). */
// d3d9-instance link 0 112 0 instance ffffffffffffffffBffffffbbbff
float4 link[8];
/* `NodeLinkUniformData`: colors[6], (aspect, arrow size). */
float4 link_uniforms[7];
float4x4 ModelViewProjectionMatrix;

struct VertIn {
  float2 uv : TEXCOORD0;
  float2 pos : TEXCOORD1;
  float2 expand : TEXCOORD2;
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
  /* line_uv, line_length, line_thickness */
  float4 line_data : TEXCOORD1;
  /* dash_length, dash_factor, dash_alpha, aspect */
  float4 dash : TEXCOORD2;
  /* has_back_link, is_main_line */
  float2 flags : TEXCOORD3;
};

float4 link_color(float id, float4 own_color)
{
  return (id < 2.5) ? own_color : link_uniforms[int(id)];
}

VertOut vert(VertIn v)
{
  const float start_gradient_threshold = 0.35;
  const float end_gradient_threshold = 0.65;

  float2 P0 = link[2].xy;
  float2 P1 = link[2].zw;
  float2 P2 = link[3].xy;
  float2 P3 = link[3].zw;
  float3 color_ids = floor(link[4].xyz * 255.0 + 0.5);
  float line_thickness = link[6].x;
  float do_arrow = link[6].z;
  float do_muted = link[6].w;
  float has_back_link = link[7].x;
  float aspect = link_uniforms[6].x;
  float arrow_size = link_uniforms[6].y;

  /* Each instance contains both the outline and the "main" line on top. */
  float is_outline_pass = v.vertex_id < 64.5;
  float expand_main = v.expand.y == 1.0;

  VertOut o;
  o.flags = float2(has_back_link, expand_main * (1.0 - is_outline_pass));
  o.dash = float4(link[5].xyz, aspect);
  /* Approximate line length, no need for real bezier length calculation. */
  o.line_data = float4(v.uv, distance(P0, P3), line_thickness);

  /* Increase width because two links are drawn. */
  line_thickness *= lerp(1.0, 1.7, expand_main * has_back_link);

  float mix_factor = saturate((v.uv.x - start_gradient_threshold) /
                              (end_gradient_threshold - start_gradient_threshold));
  float4 color = lerp(link_color(color_ids.x, link[0]), link_color(color_ids.y, link[1]),
                      mix_factor);
  color.a = lerp(color.a, 0.65, do_muted);
  color = lerp(color, link_uniforms[int(color_ids.z)], is_outline_pass);
  line_thickness *= lerp(0.65, 1.0, is_outline_pass);
  color.a *= link[5].w;
  o.color = color;

  float t = v.uv.x;
  float t2 = t * t;
  float t2_3 = 3.0 * t2;
  float one_minus_t = 1.0 - t;
  float one_minus_t2 = one_minus_t * one_minus_t;
  float one_minus_t2_3 = 3.0 * one_minus_t2;

  float2 curve_point = P0 * one_minus_t2 * one_minus_t + P1 * one_minus_t2_3 * t +
                 P2 * t2_3 * one_minus_t + P3 * t2 * t;
  float2 tangent = (P1 - P0) * one_minus_t2_3 + (P2 - P1) * 6.0 * (t - t2) + (P3 - P2) * t2_3;

  /* If the inner and outer control points overlap, the tangent is invalid: use the vector between
   * the sockets instead. */
  tangent = (dot(tangent, tangent) < 1e-12) ? P3 - P0 : tangent;
  tangent *= rsqrt(max(dot(tangent, tangent), 1e-20));
  float2 normal = float2(-tangent.y, tangent.x);

  /* Position vertex on the curve tangent space. */
  curve_point += (v.pos.x * tangent + v.pos.y * normal) * arrow_size;
  float4 clip_pos = mul(ModelViewProjectionMatrix, float4(curve_point, 0.0, 1.0));

  /* Rotate and scale the expand axis, then expand into a line. */
  float2 exp_axis = v.expand.x * tangent + v.expand.y * normal;
  exp_axis = mul(ModelViewProjectionMatrix, float4(exp_axis, 0.0, 0.0)).xy;
  float expand_dist = line_thickness * (v.uv.y * 2.0 - 1.0);
  clip_pos.xy += exp_axis * aspect * expand_dist;

  /* If the link is not muted or is not a reroute arrow the points are squashed to the center of
   * the line. Magic numbers are defined in `drawnode.cc`. */
  float squash_mute = (v.expand.x == 1.0) * (1.0 - do_muted);
  float squash_arrow = (1.0 - expand_main) * ((v.pos.x < 0.70) + (v.pos.x > 0.71)) *
                       (1.0 - do_arrow);
  clip_pos.xy *= (squash_mute + squash_arrow) > 0.0 ? 0.0 : 1.0;

  o.position = gpu_position(clip_pos);
  return o;
}

/* `smoothstep` that also works with the edges in decreasing order. */
float ramp(float edge0, float edge1, float x)
{
  float t = saturate((x - edge0) / (edge1 - edge0));
  return t * t * (3.0 - 2.0 * t);
}

#define ANTIALIAS 0.75

float get_line_alpha(float line_v, float line_thickness, float center, float relative_radius)
{
  float radius = relative_radius * line_thickness;
  float sdf = abs(line_thickness * (line_v - center));
  return ramp(radius, radius - ANTIALIAS, sdf);
}

float4 frag(VertOut v) : COLOR0
{
  float2 line_uv = v.line_data.xy;
  float line_length = v.line_data.z;
  float line_thickness = v.line_data.w;
  float dash_length = max(v.dash.x, 1e-5);
  float dash_factor = v.dash.y;

  float distance_along_line = line_length * line_uv.x;
  float dash_center = dash_length * dash_factor * 0.5;
  float normalized_distance_triangle =
      1.0 - abs(frac((distance_along_line - dash_center) / dash_length) * 2.0 - 1.0);
  float t = v.dash.w * ANTIALIAS / dash_length;
  float unclamped_alpha = 1.0 - (normalized_distance_triangle - dash_factor + t) / (2.0 * t);
  float dash_frag_alpha = (dash_factor < 1.0) ? max(v.dash.z, min(unclamped_alpha, 1.0)) : 1.0;

  float4 color = v.color;
  float single_alpha = get_line_alpha(line_uv.y, line_thickness, 0.5, 0.5) * dash_frag_alpha;

  /* Two links right next to each other, the main link and the back-link. */
  float main_alpha = color.a * get_line_alpha(line_uv.y, line_thickness, 0.75, 0.3);
  float back_alpha = get_line_alpha(line_uv.y, line_thickness, 0.2, 0.25);
  float4 double_color = float4(color.rgb * main_alpha + 0.8 * back_alpha,
                               main_alpha * dash_frag_alpha + back_alpha);

  color.a *= single_alpha;
  return (v.flags.x * v.flags.y > 0.5) ? double_color : color;
}
