/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Ember surfaces into the G-buffer (`ember_gbuffer`, see `ember_common.hlsl`). Defines: TEXTURE
 * (the base color multiplies an image texture), FLIP (back faces: flipped normals).
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"
#include "ember_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];

float4 mat_base;
float4 mat_params;
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
  o.position = gpu_position(drw_mat_mul(view_buf, DRW_WINMAT, view_pos));
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

struct FragOut {
  float4 diffuse : COLOR0;
  float4 normal : COLOR1;
  float4 depth : COLOR2;
  float4 specular : COLOR3;
};

FragOut frag(VertOut v)
{
  float3 base = mat_base.rgb;
#ifdef TEXTURE
  base *= tex2D(base_tx, v.uv).rgb;
#endif
  float metallic = mat_params.x;
  FragOut o;
  o.diffuse = float4(sqrt(base * (1.0 - metallic)), mat_params.y);
  o.normal = float4(normalize(v.normal_depth.xyz) * 0.5 + 0.5, 0.0);
  o.depth = v.normal_depth.wwww;
  o.specular = float4(sqrt(lerp(mat_params.zzz, base, metallic)), mat_params.w);
  return o;
}
