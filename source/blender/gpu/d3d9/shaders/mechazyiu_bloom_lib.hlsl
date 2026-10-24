/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU bloom levels: RGBM encoded colors (color = rgb * a * range) and the tent filter that
 * adds them up (`mechazyiu_bloom.hlsl`, `mechazyiu_copy.hlsl`).
 */

#define MECHAZYIU_BLOOM_RANGE 64.0

float3 mechazyiu_rgbm_decode(float4 e)
{
  return e.rgb * (e.a * MECHAZYIU_BLOOM_RANGE);
}

float4 mechazyiu_rgbm_encode(float3 color)
{
  float m = clamp(max(max(color.r, color.g), color.b) * (1.0 / MECHAZYIU_BLOOM_RANGE),
                  1.0 / 255.0,
                  1.0);
  return float4(saturate(color / (m * MECHAZYIU_BLOOM_RANGE)), m);
}

/**
 * Tent filter of 9 bilinear samples of `tx`, `texel.z` texels (`texel.xy`: texel size) apart.
 */
float3 mechazyiu_bloom_upsample(sampler2D tx, float2 uv, float4 texel)
{
  float4 d = texel.xyxy * float4(1.0, 1.0, -1.0, 0.0) * texel.z;
  float3 s = mechazyiu_rgbm_decode(tex2D(tx, uv)) * 4.0;
  s += (mechazyiu_rgbm_decode(tex2D(tx, uv - d.wy)) + mechazyiu_rgbm_decode(tex2D(tx, uv + d.zw)) +
        mechazyiu_rgbm_decode(tex2D(tx, uv + d.xw)) + mechazyiu_rgbm_decode(tex2D(tx, uv + d.wy))) *
       2.0;
  s += mechazyiu_rgbm_decode(tex2D(tx, uv - d.xy)) + mechazyiu_rgbm_decode(tex2D(tx, uv - d.zy)) +
       mechazyiu_rgbm_decode(tex2D(tx, uv + d.zy)) + mechazyiu_rgbm_decode(tex2D(tx, uv + d.xy));
  return s * (1.0 / 16.0);
}
