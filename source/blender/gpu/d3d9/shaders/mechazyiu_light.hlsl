/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * One light of an MECHAZYIU sample (`mechazyiu_light_*`), added to the running mean over the rectangle
 * the light reaches. Defines: SUN, SPOT, AREA (point light without any). BLEND: the contribution is
 * added by blending (hardware blending into floating point targets), instead of being added to
 * the sample read from `accum_tx`.
 *
 * Lambert diffuse and GGX specular (Kelemen visibility, Schlick Fresnel): the cheaper terms keep
 * it below the ps_2_0 instruction limit.
 * Light sizes are integrated by the samples: each one moves the light to another point of its
 * shape, like the soft shadows.
 */

#include "d3d9_common.hlsl"
#define MECHAZYIU_SCREEN_PASS
#include "mechazyiu_common.hlsl"

sampler2D gb_diffuse_tx : register(s0);
sampler2D gb_normal_tx : register(s1);
sampler2D gb_depth_tx : register(s2);
sampler2D gb_specular_tx : register(s3);
#ifndef BLEND
sampler2D accum_tx : register(s4);
#endif
/* Shadow of the light (`mechazyiu_shadow_mask.hlsl`), or a white texture. */
sampler2D shadow_mask_tx : register(s5);

float4 light_position;
float4 light_direction;
float4 light_color;
float4 light_spot;

float4 frag(VertOut v) : COLOR0
{
  float4 gd = tex2D(gb_diffuse_tx, v.uv);
  float3 n = mechazyiu_normal_decode(tex2D(gb_normal_tx, v.uv).xyz);
  float depth = tex2D(gb_depth_tx, v.uv).x;
  float4 gs = tex2D(gb_specular_tx, v.uv);
#ifdef BLEND
  float4 accum = 0.0;
#else
  float4 accum = tex2D(accum_tx, v.uv);
#endif
  float shadow = tex2D(shadow_mask_tx, v.uv).x;

#ifdef SUN
  float3 L = light_position.xyz;
  float attenuation = 1.0;
#else
  float3 P = v.ray_origin + v.ray_dir * depth;
  float3 Lv = light_position.xyz - P;
  float dist2 = dot(Lv, Lv);
  float inv_dist = rsqrt(dist2);
  float3 L = Lv * inv_dist;
  /* Inverse square falloff, offset to reach 0 at the influence radius. */
  float attenuation = max(inv_dist * inv_dist - light_position.w, 0.0);
#  ifdef SPOT
  float spot = saturate(dot(-L, light_direction.xyz) * light_spot.x + light_spot.y);
  attenuation *= spot * spot * (3.0 - 2.0 * spot);
#  endif
#  ifdef AREA
  attenuation *= saturate(dot(-L, light_direction.xyz));
#  endif
#endif

  float3 V = -normalize(v.ray_dir);
  float3 H = normalize(L + V);
  float NL = saturate(dot(n, L));
  float NH = saturate(dot(n, H));
  float VH = saturate(dot(V, H));

  float a = gd.a * gd.a;
  float a2 = a * a;
  float d = NH * NH * (a2 - 1.0) + 1.0;
  /* D * visibility (4 is in the light color). */
  float D_vis = a2 / (d * d * max(VH * VH, 0.01));
  float3 f0 = gs.rgb * gs.rgb;
  float fc = 1.0 - VH;
  float fc2 = fc * fc;
  fc = fc2 * fc2 * fc * saturate(50.0 * f0.g);
  float3 F = f0 + (1.0 - f0) * fc;

  float3 brdf = gd.rgb * gd.rgb * light_spot.z + F * (D_vis * light_color.w);
  return accum + float4(brdf * light_color.rgb * (NL * attenuation * shadow), 0.0);
}
