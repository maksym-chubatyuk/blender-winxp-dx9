/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * First pass of an MECHAZYIU sample (`mechazyiu_base`): world lighting, emission and the background,
 * mixed into the running mean of the previous samples.
 */

#include "d3d9_common.hlsl"
#define MECHAZYIU_SCREEN_PASS
#include "mechazyiu_common.hlsl"

sampler2D gb_diffuse_tx : register(s0);
sampler2D gb_normal_tx : register(s1);
sampler2D gb_depth_tx : register(s2);
sampler2D gb_specular_tx : register(s3);
sampler2D accum_tx : register(s4);
sampler2D material_tx : register(s5);

float4 world_sh[4];
float4 background;
float4 accum_uv;

float3 world_light(float3 n)
{
  return max(world_sh[0].rgb + n.x * world_sh[1].rgb + n.y * world_sh[2].rgb +
                 n.z * world_sh[3].rgb,
             0.0);
}

float4 frag(VertOut v) : COLOR0
{
  float4 gd = tex2D(gb_diffuse_tx, v.uv);
  float3 n = mechazyiu_normal_decode(tex2D(gb_normal_tx, v.uv).xyz);
  float depth = tex2D(gb_depth_tx, v.uv).x;
  float4 gs = tex2D(gb_specular_tx, v.uv);
  float4 prev = tex2D(accum_tx, v.uv * accum_uv.xy + accum_uv.zw);
  float3 emission = tex2D(material_tx, float2(gs.a * (255.0 / 256.0) + 0.5 / 256.0, 0.5)).rgb;

  float3 V = -normalize(v.ray_dir);
  float NV = saturate(dot(n, V));
  float3 R = n * (2.0 * dot(n, V)) - V;

  /* Split sum approximation of the environment's specular reflection (Karis, mobile). */
  float roughness = gd.a;
  float4 r = roughness * float4(-1.0, -0.0275, -0.572, 0.022) + float4(1.0, 0.0425, 1.04, -0.04);
  float a004 = min(r.x * r.x, exp2(-9.28 * NV)) * r.x + r.y;
  float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
  float3 f0 = gs.rgb * gs.rgb;
  float3 spec = f0 * ab.x + saturate(50.0 * f0.g) * ab.y;

  float3 color = gd.rgb * gd.rgb * world_light(n) + spec * world_light(R) + emission;
  float4 result = depth > 0.0 ? float4(color, 1.0) : float4(background.rgb, 1.0 - world_sh[0].a);
  return lerp(prev, result, background.a);
}
