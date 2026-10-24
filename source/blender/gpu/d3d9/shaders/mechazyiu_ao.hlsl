/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Ambient occlusion of a MECHAZYIU sample (`mechazyiu_ao`), before the base pass that multiplies
 * the world lighting by it: 4 points of the G-buffer around the pixel, within `ao_params.x` (view
 * space distance), occlude by how much they are above the surface (cosine to the normal) and how
 * near they are. The points turn and change distance with every sample (`ao_taps`, the same for
 * all pixels: no noise pattern, the samples average to a smooth result).
 */

#include "d3d9_common.hlsl"
#define MECHAZYIU_SCREEN_PASS
#include "mechazyiu_common.hlsl"

sampler2D gb_normal_tx : register(s1);
sampler2D gb_depth_tx : register(s2);

/* x: 1 / distance^2 (view space), y: factor / 2 (twice the mean of the 4 points). */
float4 ao_params;
/* Offsets of the 4 points in texture coordinates (xy and zw, two per vector), divided by the depth
 * in perspective. */
float4 ao_taps[2];
/* View space offset per texture coordinate offset (xy), at depth 1 in perspective. */
float4 ao_uv_to_view;

/**
 * Occlusion by the point of view ray `r` (perspective: at depth 1, orthographic: its position
 * without depth) at depth `depth`, for the pixel at `p` of normal `n`: by how much it is above
 * the surface (not the surface itself: its flat neighbors) and how near it is (not the far points,
 * nor the background).
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

float4 frag(VertOut v) : COLOR0
{
  float depth = tex2D(gb_depth_tx, v.uv).x;
  float3 n = mechazyiu_normal_decode(tex2D(gb_normal_tx, v.uv).xyz);
  float3 p = v.ray_origin + v.ray_dir * depth;
#ifdef ORTHO
  float4 base = v.ray_origin.xyxy;
  float4 off0 = ao_taps[0];
  float4 off1 = ao_taps[1];
#else
  /* The offsets shrink with the distance. */
  float4 base = v.ray_dir.xyxy;
  float scale = 1.0 / depth;
  float4 off0 = ao_taps[0] * scale;
  float4 off1 = ao_taps[1] * scale;
#endif
  float4 r0 = base + off0 * ao_uv_to_view.xyxy;
  float4 r1 = base + off1 * ao_uv_to_view.xyxy;
  float o = occlusion(r0.xy, tex2D(gb_depth_tx, v.uv + off0.xy).x, p, n) +
            occlusion(r0.zw, tex2D(gb_depth_tx, v.uv + off0.zw).x, p, n) +
            occlusion(r1.xy, tex2D(gb_depth_tx, v.uv + off1.xy).x, p, n) +
            occlusion(r1.zw, tex2D(gb_depth_tx, v.uv + off1.zw).x, p, n);
  return saturate(1.0 - o * ao_params.y).xxxx;
}
