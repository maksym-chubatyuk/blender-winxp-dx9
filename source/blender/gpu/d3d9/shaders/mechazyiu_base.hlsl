/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * First pass of a MECHAZYIU sample (`mechazyiu_base`): world lighting, emission and the
 * background (premultiplied alpha).
 */

#include "d3d9_common.hlsl"
#define MECHAZYIU_SCREEN_PASS
#include "mechazyiu_common.hlsl"

sampler2D gb_diffuse_tx : register(s0);
sampler2D gb_normal_tx : register(s1);
sampler2D gb_depth_tx : register(s2);
sampler2D gb_specular_tx : register(s3);
sampler2D material_tx : register(s5);
samplerCUBE world_tx : register(s6);
samplerCUBE world_glossy_tx : register(s7);
samplerCUBE world_rough_tx : register(s8);
samplerCUBE irradiance_tx : register(s9);

float4 world_rotation[3];
float4 world_params;

float3 world_direction(float3 dir)
{
  return float3(dot(world_rotation[0].xyz, dir),
                dot(world_rotation[1].xyz, dir),
                dot(world_rotation[2].xyz, dir));
}

/* RGBM, without the range (`world_params.x`, applied once). */
float3 rgbm(float4 e)
{
  return e.rgb * e.a;
}

float4 frag(VertOut v) : COLOR0
{
  float4 gd = tex2D(gb_diffuse_tx, v.uv);
  float3 n = mechazyiu_normal_decode(tex2D(gb_normal_tx, v.uv).xyz);
  float depth = tex2D(gb_depth_tx, v.uv).x;
  float4 gs = tex2D(gb_specular_tx, v.uv);
  float3 emission = tex2D(material_tx, float2(gs.a * (255.0 / 256.0) + 0.5 / 256.0, 0.5)).rgb;

  /* In the environment's space. */
  float3 ray = world_direction(v.ray_dir);
  n = world_direction(n);
  float3 V = -normalize(ray);
  float NV = saturate(dot(n, V));
  float3 R = n * (2.0 * dot(n, V)) - V;

  /* Split sum approximation of the environment's specular reflection (Karis, mobile). */
  float roughness = gd.a;
  float4 r = roughness * float4(-1.0, -0.0275, -0.572, 0.022) + float4(1.0, 0.0425, 1.04, -0.04);
  float a004 = min(r.x * r.x, exp2(-9.28 * NV)) * r.x + r.y;
  float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
  float3 f0 = gs.rgb * gs.rgb;
  float3 spec = f0 * ab.x + saturate(50.0 * f0.g) * ab.y;

  /* Reflection of the environment, between the maps of the nearest roughnesses. The background
   * is looked up the same way (blurred like a reflection). */
  bool surface = depth > 0.0;
  float3 dir = surface ? R : ray;
  float4 w = saturate(1.0 - abs((surface ? roughness : world_params.z) * 3.0 -
                                float4(0.0, 1.0, 2.0, 3.0)));
  float3 env = (rgbm(texCUBE(world_tx, dir)) * w.x + rgbm(texCUBE(world_glossy_tx, dir)) * w.y +
                rgbm(texCUBE(world_rough_tx, dir)) * w.z +
                rgbm(texCUBE(irradiance_tx, dir)) * w.w) *
               world_params.x;
  float3 irradiance = rgbm(texCUBE(irradiance_tx, n)) * world_params.x;
  float3 color = gd.rgb * gd.rgb * irradiance + spec * env + emission;
  return surface ? float4(color, 1.0) : float4(env, 1.0) * world_params.w;
}
