/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Ambient occlusion and bounce light of a MECHAZYIU sample, before the base pass that uses them
 * for the world lighting. 4 points of the G-buffer around the pixel, within the occlusion
 * distance, occlude by how much they are above the surface and how near they are. The points turn
 * and change distance with every sample (`ao_taps`, the same for all pixels: no noise pattern, the
 * samples average to a smooth result).
 *
 * `mechazyiu_ao` (ORTHO: orthographic views): the weights of the 4 points (rgba).
 * `mechazyiu_bounce` (BOUNCE): from the weights, the occlusion (a) and the bounce light (rgb,
 * irradiance / pi): the occluded part of the world's light replaced by the light of the points,
 * the mean of the previous samples (`mean_tx`: one more bounce every sample).
 */

#include "d3d9_common.hlsl"
#define MECHAZYIU_SCREEN_PASS
#include "mechazyiu_common.hlsl"

sampler2D gb_normal_tx : register(s1);
sampler2D gb_depth_tx : register(s2);
#ifdef BOUNCE
sampler2D mean_tx : register(s6);
sampler2D weights_tx : register(s5);
#endif

/* x: 1 / distance^2 (view space), y: factor / 2 (twice the mean of the 4 points: surrounded
 * pixels, half of the points below the surface, are fully occluded), z: bounce light factor (0:
 * none), w: 1 for perspective. */
float4 ao_params;
/* Offsets of the 4 points in texture coordinates (xy and zw, two per vector), divided by the depth
 * in perspective. */
float4 ao_taps[2];
/* View space offset per texture coordinate offset (xy), at depth 1 in perspective. */
float4 ao_uv_to_view;
/* Texture coordinates of `mean_tx` from the G-buffer's (xy scale, zw offset). */
float4 mean_uv_transform;

/**
 * Weight of the point of view ray `r` (perspective: at depth 1, orthographic: its position without
 * depth) at depth `depth`, for the pixel at `p` of normal `n`: by how much it is above the surface
 * (not the surface itself: its flat neighbors) and how near it is (not the far points, nor the
 * background).
 */
float occlusion(float2 r, float depth, float3 p, float3 n)
{
#ifdef ORTHO
  float3 v = float3(r, -depth) - p;
#else
  float3 v = float3(r, -1.0) * depth - p;
#endif
  float len2 = dot(v, v);
  return saturate(dot(n, v) * rsqrt(len2) - 0.1) * saturate(1.0 - len2 * ao_params.x);
}

#ifdef BOUNCE
float4 frag(VertOut v) : COLOR0
{
  float depth = tex2D(gb_depth_tx, v.uv).x;
  float4 w = tex2D(weights_tx, v.uv);
  float scale = lerp(1.0, 1.0 / depth, ao_params.w);
  float4 m0 = (v.uv.xyxy + ao_taps[0] * scale) * mean_uv_transform.xyxy + mean_uv_transform.zwzw;
  float4 m1 = (v.uv.xyxy + ao_taps[1] * scale) * mean_uv_transform.xyxy + mean_uv_transform.zwzw;
  float3 light = tex2D(mean_tx, m0.xy).rgb * w.x + tex2D(mean_tx, m0.zw).rgb * w.y +
                 tex2D(mean_tx, m1.xy).rgb * w.z + tex2D(mean_tx, m1.zw).rgb * w.w;
  /* The occluders' mean light times the occluded fraction: their weighted sum times the factor of
   * the occlusion. */
  return float4(light * ao_params.z, saturate(1.0 - dot(w, ao_params.yyyy)));
}
#else
float4 frag(VertOut v) : COLOR0
{
  float depth = tex2D(gb_depth_tx, v.uv).x;
  float3 n = mechazyiu_normal_decode(tex2D(gb_normal_tx, v.uv).xyz);
  float3 p = v.ray_origin + v.ray_dir * depth;
#  ifdef ORTHO
  float4 base = v.ray_origin.xyxy;
  float4 off0 = ao_taps[0];
  float4 off1 = ao_taps[1];
#  else
  /* The offsets shrink with the distance. */
  float4 base = v.ray_dir.xyxy;
  float scale = 1.0 / depth;
  float4 off0 = ao_taps[0] * scale;
  float4 off1 = ao_taps[1] * scale;
#  endif
  float4 r0 = base + off0 * ao_uv_to_view.xyxy;
  float4 r1 = base + off1 * ao_uv_to_view.xyxy;
  return float4(occlusion(r0.xy, tex2D(gb_depth_tx, v.uv + off0.xy).x, p, n),
                occlusion(r0.zw, tex2D(gb_depth_tx, v.uv + off0.zw).x, p, n),
                occlusion(r1.xy, tex2D(gb_depth_tx, v.uv + off1.xy).x, p, n),
                occlusion(r1.zw, tex2D(gb_depth_tx, v.uv + off1.zw).x, p, n));
}
#endif
