/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU blended surfaces (material render method "Blended"), drawn back to front over the
 * lit sample like the games of the time: `mechazyiu_forward` puts the surface over it with the
 * world light and emission, then `mechazyiu_forward_light` (define LIGHT) adds each light (no
 * shadows on them, normalized Blinn-Phong specular: GGX doesn't fit). Surfaces facing away are lit
 * from their other side.
 *
 * Defines: TEXTURE (the base color multiplies an image), BLEND: blended by the hardware
 * (premultiplied alpha, then adding), else the sample is read (`sample_tx`) and written to the
 * other buffer.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"
#include "mechazyiu_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];

/* rgb: base color, a: alpha. */
float4 mat_base;
/* metallic, Blinn-Phong exponent of the roughness, dielectric F0, 1 when the alpha of the texture
 * is the coverage. */
float4 mat_params;
/* x: 1 for perspective projections, zw: part of the textures used (tile buffers). */
float4 proj_type;
#ifdef LIGHT
/* View space position, or direction toward the light for suns (normalized, w = 0, and
 * `light_direction.w` = 0: the falloff is then 1). */
float4 light_position;
/* xyz: view space direction the light points to, w: 1 / influence radius squared. */
float4 light_direction;
/* rgb: radiant intensity / pi, w: specular factor / 4. */
float4 light_color;
/* Cone factor `c = saturate(dot(-L, direction) * x + y)`, attenuation `c * lerp(1, c, z)`
 * (spots: z = 1, areas: the cosine, z = 0, others: x = 0, y = 1). w: diffuse factor. */
float4 light_spot;
#else
float4 world_sh[4];
/* rgb: emission. */
float4 emission;
#endif

#ifdef TEXTURE
sampler2D base_tx : register(s0);
#endif
#ifndef BLEND
sampler2D sample_tx : register(s1);
#endif

struct VertIn {
  float3 pos : TEXCOORD0;
  float3 nor : TEXCOORD1;
#ifdef TEXTURE
  float2 au : TEXCOORD2;
#endif
};

struct VertOut {
  float4 position : POSITION;
  float3 normal : TEXCOORD0;
  float3 view_pos : TEXCOORD1;
  /* Toward the eye (not normalized). */
  float3 view_dir : TEXCOORD4;
  /* Projective texture coordinates of the pixel in the tile buffers. */
  float4 screen_uv : TEXCOORD2;
#ifdef TEXTURE
  float2 uv : TEXCOORD3;
#endif
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  float4 view_pos = drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0));
  float4 clip = drw_mat_mul(view_buf, DRW_WINMAT, view_pos);
  o.position = gpu_position(clip);
  o.normal = drw_mat_mul3(view_buf, DRW_VIEWMAT, drw_normal_object_to_world(v.nor));
  o.view_pos = view_pos.xyz;
  o.view_dir = lerp(float3(0.0, 0.0, 1.0), -view_pos.xyz, proj_type.x);
  o.screen_uv = float4((clip.xy * 0.5 + clip.w * 0.5) * proj_type.zw, 0.0, clip.w);
#ifdef TEXTURE
  o.uv = v.au;
#endif
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  float4 base = mat_base;
#ifdef TEXTURE
  float4 texel = tex2D(base_tx, v.uv);
  base.rgb *= texel.rgb;
  base.a *= lerp(1.0, texel.a, mat_params.w);
#endif
  float3 V = normalize(v.view_dir);
  float3 n = normalize(v.normal);
  n = dot(n, V) < 0.0 ? -n : n;
  float metallic = mat_params.x;
  float3 diffuse = base.rgb * (1.0 - metallic);

#ifdef LIGHT
  float3 Lv = light_position.xyz - v.view_pos * light_position.w;
  float inv_dist = rsqrt(dot(Lv, Lv));
  float3 L = Lv * inv_dist;
  float attenuation = max(inv_dist * inv_dist - light_direction.w, 0.0);
  float cone = saturate(dot(-L, light_direction.xyz) * light_spot.x + light_spot.y);
  attenuation *= cone * lerp(1.0, cone, light_spot.z);
  float3 H = normalize(L + V);
  float NL = saturate(dot(n, L));
  float NH = saturate(dot(n, H));
  float VH = saturate(dot(V, H));
  /* Normalized Blinn-Phong, times 4 (the specular factor is divided by 4). */
  float D_vis = pow(NH, mat_params.y) * (mat_params.y * 0.5 + 4.0);
  float3 f0 = lerp(mat_params.zzz, base.rgb, metallic);
  float fc = 1.0 - VH;
  float fc2 = fc * fc;
  float3 F = f0 + (1.0 - f0) * (fc2 * fc2 * fc);
  float3 color = (diffuse * light_spot.w + F * (D_vis * light_color.w)) * light_color.rgb *
                 (NL * attenuation);
  float4 layer = float4(color * base.a, 0.0);
#else
  float3 sh = world_sh[0].rgb + n.x * world_sh[1].rgb + n.y * world_sh[2].rgb +
              n.z * world_sh[3].rgb;
  float3 color = diffuse * max(sh, 0.0) + emission.rgb;
  /* Premultiplied: over the sample. */
  float4 layer = float4(color * base.a, base.a);
#endif

#ifdef BLEND
  return layer;
#else
  float4 prev = tex2Dproj(sample_tx, v.screen_uv);
#  ifdef LIGHT
  return prev + layer;
#  else
  return float4(prev.rgb * (1.0 - layer.a) + layer.rgb, prev.a * (1.0 - layer.a) + layer.a);
#  endif
#endif
}
