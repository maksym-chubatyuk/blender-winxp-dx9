/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU surfaces into the G-buffer (`mechazyiu_gbuffer`, see `mechazyiu_common.hlsl`).
 * Defines: TEXTURE (the base color multiplies an image texture), FLIP (back faces: flipped
 * normals), SINGLE (one target per draw, on hardware without multiple render targets:
 * `gbuffer_target` selects it).
 *
 * ALPHA_PREPASS (`mechazyiu_alpha_prepass`): depth of the cutout surfaces (image alpha, alpha
 * tested at 0.5); their G-buffer pass then only draws where its depth is equal. The vertex shader
 * is the same, for the same depths.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"
#include "mechazyiu_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];

float4 mat_base;
float4 mat_params;
#ifdef SINGLE
/* One-hot selection of the target written. */
float4 gbuffer_target;
#endif
#ifdef ALPHA_PREPASS
// d3d9-alpha-test
#endif
#ifdef TEXTURE
sampler2D base_tx : register(s0);
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
  float4 normal_depth : TEXCOORD0;
#ifdef TEXTURE
  float2 uv : TEXCOORD1;
#endif
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  float4 view_pos = drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0));
  float4 clip = drw_mat_mul(view_buf, DRW_WINMAT, view_pos);
  o.position = gpu_position(clip);

  float3 n = drw_mat_mul3(view_buf, DRW_VIEWMAT, drw_normal_object_to_world(v.nor));
#ifdef FLIP
  n = -n;
#endif
  o.normal_depth = float4(n, -view_pos.z);
#ifdef TEXTURE
  o.uv = v.au;
#endif
  return o;
}

#ifdef ALPHA_PREPASS
float4 frag(VertOut v) : COLOR0
{
#  ifdef TEXTURE
  return float4(0.0, 0.0, 0.0, tex2D(base_tx, v.uv).a);
#  else
  return 1.0;
#  endif
}
#else

struct FragOut {
  float4 diffuse : COLOR0;
#ifndef SINGLE
  float4 normal : COLOR1;
  float4 depth : COLOR2;
  float4 specular : COLOR3;
#endif
};

struct GBuffer {
  float4 diffuse;
  float4 normal;
  float4 depth;
  float4 specular;
};

FragOut frag(VertOut v)
{
  float3 base = mat_base.rgb;
#ifdef TEXTURE
  base *= tex2D(base_tx, v.uv).rgb;
#endif
  float metallic = mat_params.x;
  GBuffer g;
  g.diffuse = float4(sqrt(base * (1.0 - metallic)), mat_params.y);
  g.normal = float4(normalize(v.normal_depth.xyz) * 0.5 + 0.5, 0.0);
  g.depth = v.normal_depth.wwww;
  g.specular = float4(sqrt(lerp(mat_params.zzz, base, metallic)), mat_params.w);
  FragOut o;
#ifdef SINGLE
  o.diffuse = g.diffuse * gbuffer_target.x + g.normal * gbuffer_target.y +
              g.depth * gbuffer_target.z + g.specular * gbuffer_target.w;
#else
  o.diffuse = g.diffuse;
  o.normal = g.normal;
  o.depth = g.depth;
  o.specular = g.specular;
#endif
  return o;
}
#endif
