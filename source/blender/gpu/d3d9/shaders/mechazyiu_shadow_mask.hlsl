/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU shadow of one light (`mechazyiu_shadow_mask_*`): 1 where the G-buffer surface sees the light,
 * over the rectangle the light reaches. Defines: CUBE (point and area lights: 6 faces of 90
 * degrees around the light, aligned with the view axes), ORTHO (suns), none (spots).
 *
 * Shadow map atlas: CUBE faces are cells of a 4 x 2 grid (face 2 * axis + negative), the others
 * use the left half. One lookup per sample, moved by a sub-texel offset each sample: the samples
 * average into filtered edges.
 */

#include "d3d9_common.hlsl"
#define MECHAZYIU_SCREEN_PASS
#include "mechazyiu_common.hlsl"

sampler2D gb_normal_tx : register(s1);
sampler2D gb_depth_tx : register(s2);
sampler2D shadow_tx : register(s6);

/* View space light position (CUBE), w: 1 / far distance. */
float4 light_position;
/* Spot and sun: view space to the light's clip space. */
float4 shadow_mat[4];
/* xy: lookup offset (atlas uv) for this sample, z: normal offset per unit of distance to the
 * light (CUBE, spot), w: constant normal offset (sun). */
float4 shadow_params;
/* Depth bias: the surface is lit when `ref * x - y <= occluder`. */
float4 shadow_bias;

float4 frag(VertOut v) : COLOR0
{
  float3 n = mechazyiu_normal_decode(tex2D(gb_normal_tx, v.uv).xyz);
  float depth = tex2D(gb_depth_tx, v.uv).x;
  float3 P = v.ray_origin + v.ray_dir * depth;

#ifdef CUBE
  float3 to_light = P - light_position.xyz;
  /* Move the point along its normal by about a texel, against acne. */
  P += n * (length(to_light) * shadow_params.z);
  float3 d = P - light_position.xyz;
  float3 a = abs(d);
  /* One-hot major axis. */
  float is_x = (a.x >= a.y && a.x >= a.z) ? 1.0 : 0.0;
  float is_y = (1.0 - is_x) * (a.y >= a.z ? 1.0 : 0.0);
  float is_z = 1.0 - is_x - is_y;
  float3 axis = float3(is_x, is_y, is_z);
  float major = dot(a, axis);
  float2 face_uv = (is_x * d.yz + is_y * d.xz + is_z * d.xy) / major * 0.5 + 0.5;
  float negative = dot(d, axis) < 0.0 ? 1.0 : 0.0;
  /* Cell of the face: x = (2 * axis + negative) % 4, y = (2 * axis + negative) / 4. */
  float face = dot(axis, float3(0.0, 2.0, 4.0)) + negative;
  float2 cell = float2(face - is_z * 4.0, is_z);
  float2 uv = (cell + face_uv) * float2(0.25, 0.5) + shadow_params.xy;
  float ref = length(d) * light_position.w;
#else
#  ifdef ORTHO
  P += n * shadow_params.w;
#  else
  P += n * (length(P - light_position.xyz) * shadow_params.z);
#  endif
  float4 clip = shadow_mat[0] * P.x + shadow_mat[1] * P.y + shadow_mat[2] * P.z + shadow_mat[3];
#  ifdef ORTHO
  float ref = clip.z * 0.5 + 0.5;
#  else
  float ref = length(P - light_position.xyz) * light_position.w;
  clip.xy /= clip.w;
#  endif
  float2 uv = (clip.xy * 0.5 + 0.5) * float2(0.5, 1.0) + shadow_params.xy;
#endif
  float occluder = tex2D(shadow_tx, uv).x;
  return (ref * shadow_bias.x - shadow_bias.y <= occluder ? 1.0 : 0.0).xxxx;
}
