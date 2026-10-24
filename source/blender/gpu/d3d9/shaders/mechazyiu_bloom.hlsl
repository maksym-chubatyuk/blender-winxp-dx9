/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU bloom of the viewport (final renders: `mechazyiu_bloom.cc`, the same filters, those of
 * EEVEE Legacy). The levels are RGBM encoded 8 bit colors (range MECHAZYIU_BLOOM_RANGE): bilinear
 * filtering of 8 bit textures works on all hardware, of float textures not.
 *
 * - PREFILTER (`mechazyiu_bloom_prefilter`): the bright parts of the mean (`source_tx`, float) at
 *   half the resolution.
 * - DOWNSAMPLE (`mechazyiu_bloom_downsample`): half the resolution of `source_tx`.
 * - UPSAMPLE (`mechazyiu_bloom_upsample`): `base_tx` plus `source_tx` (the lower level), tent
 *   filtered.
 * The last level is upsampled once more to half the resolution (UPSAMPLE, black base), that the
 * viewport output adds (`mechazyiu_output_bloom`).
 */

#include "d3d9_common.hlsl"
#define MECHAZYIU_SCREEN_PASS
#include "mechazyiu_common.hlsl"
#include "mechazyiu_bloom_lib.hlsl"

sampler2D source_tx : register(s0);
#ifdef UPSAMPLE
sampler2D base_tx : register(s1);
#endif
/* xy: texel size of `source_tx`, z: distance of the tent filter's samples (in texels). */
float4 bloom_texel;
#ifdef PREFILTER
/* Threshold curve: threshold - knee, knee * 2, 0.25 / knee, threshold. */
float4 bloom_curve;
/* x: clamp (0: none). */
float4 bloom_params;
#endif

/* 4 bilinear samples one texel away diagonally: the mean of 4 x 4 texels. */
float3 downsample(float2 uv)
{
  float4 d = bloom_texel.xyxy * float4(-1.0, -1.0, 1.0, 1.0);
#ifdef PREFILTER
  /* Float colors. */
  return (tex2D(source_tx, uv + d.xy).rgb + tex2D(source_tx, uv + d.zy).rgb +
          tex2D(source_tx, uv + d.xw).rgb + tex2D(source_tx, uv + d.zw).rgb) *
         0.25;
#else
  return (mechazyiu_rgbm_decode(tex2D(source_tx, uv + d.xy)) +
          mechazyiu_rgbm_decode(tex2D(source_tx, uv + d.zy)) +
          mechazyiu_rgbm_decode(tex2D(source_tx, uv + d.xw)) +
          mechazyiu_rgbm_decode(tex2D(source_tx, uv + d.zw))) *
         0.25;
#endif
}

float4 frag(VertOut v) : COLOR0
{
#ifdef UPSAMPLE
  float3 color = mechazyiu_rgbm_decode(tex2D(base_tx, v.uv)) +
                 mechazyiu_bloom_upsample(source_tx, v.uv, bloom_texel);
#else
  float3 color = downsample(v.uv);
#endif
#ifdef PREFILTER
  float brightness = max(max(max(color.r, color.g), color.b), 1e-5);
  /* Under the threshold: a quadratic curve (the knee). */
  float soft = clamp(brightness - bloom_curve.x, 0.0, bloom_curve.y);
  soft = bloom_curve.z * soft * soft;
  color *= max(soft, brightness - bloom_curve.w) / brightness;
  if (bloom_params.x > 0.0) {
    float clamped = max(max(max(color.r, color.g), color.b), 1e-5);
    color *= 1.0 - max(0.0, clamped - bloom_params.x) / clamped;
  }
#endif
  return mechazyiu_rgbm_encode(color);
}
