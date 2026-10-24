/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU surfaces into the G-buffer (`mechazyiu_gbuffer`, see `mechazyiu_common.hlsl`).
 * Defines: TEXTURE (the base color multiplies an image texture), MAPS (TEXTURE, and images for the
 * normal (tangent space), the roughness and the metallic: unused ones are 1 x 1 neutral images),
 * FLIP (back faces: flipped normals), SINGLE (one target per draw, on hardware without multiple
 * render targets: `gbuffer_target` selects it).
 *
 * ALPHA_PREPASS (`mechazyiu_alpha_prepass`): depth of the cutout surfaces (an image's channel,
 * alpha tested at 0.5); their G-buffer pass then only draws where its depth is equal. The vertex
 * shader is the same, for the same depths.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"
#include "mechazyiu_common.hlsl"

#ifdef MAPS
#  define TEXTURE
#endif

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
/* One-hot selection of the channel of `base_tx` holding the coverage. */
float4 coverage_channel;
#endif
#ifdef MAPS
/* One-hot selections of the channels of the roughness and metallic images (they multiply
 * `mat_params`). x of `mat_maps`: strength of the normal map. */
float4 roughness_channel;
float4 metallic_channel;
float4 mat_maps;
sampler2D normal_tx : register(s1);
sampler2D roughness_tx : register(s2);
sampler2D metallic_tx : register(s3);
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
#ifdef MAPS
  float4 at : TEXCOORD3;
#endif
};

struct VertOut {
  float4 position : POSITION;
  float4 normal_depth : TEXCOORD0;
#ifdef TEXTURE
  float2 uv : TEXCOORD1;
#endif
#ifdef MAPS
  /* View space tangent, w: sign of the bitangent. */
  float4 tangent : TEXCOORD2;
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
#ifdef MAPS
  float3 world_tangent = drw_model[0].xyz * v.at.x + drw_model[1].xyz * v.at.y +
                         drw_model[2].xyz * v.at.z;
  o.tangent = float4(drw_mat_mul3(view_buf, DRW_VIEWMAT, world_tangent), v.at.w);
#endif
  return o;
}

#ifdef ALPHA_PREPASS
float4 frag(VertOut v) : COLOR0
{
  return float4(0.0, 0.0, 0.0, dot(tex2D(base_tx, v.uv), coverage_channel));
}
#else

struct FragOut {
  float4 diffuse : COLOR0;
#  ifndef SINGLE
  float4 normal : COLOR1;
  float4 depth : COLOR2;
  float4 specular : COLOR3;
#  endif
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
#  ifdef TEXTURE
  base *= tex2D(base_tx, v.uv).rgb;
#  endif
  float metallic = mat_params.x;
  float roughness = mat_params.y;
  float3 n = normalize(v.normal_depth.xyz);
#  ifdef MAPS
  metallic *= dot(tex2D(metallic_tx, v.uv), metallic_channel);
  roughness = max(roughness * dot(tex2D(roughness_tx, v.uv), roughness_channel), 0.045);
  /* Tangent frame (Gram-Schmidt), the image's normal in it, mixed by the strength. */
  float3 t = normalize(v.tangent.xyz - n * dot(n, v.tangent.xyz));
  float3 b = cross(n, t) * v.tangent.w;
  float3 mapped = tex2D(normal_tx, v.uv).xyz * 2.0 - 1.0;
  mapped = t * mapped.x + b * mapped.y + n * mapped.z;
  n = normalize(lerp(n, mapped, mat_maps.x));
#  endif
  GBuffer g;
  g.diffuse = float4(sqrt(base * (1.0 - metallic)), roughness);
  g.normal = float4(n * 0.5 + 0.5, 0.0);
  g.depth = v.normal_depth.wwww;
  g.specular = float4(sqrt(lerp(mat_params.zzz, base, metallic)), mat_params.w);
  FragOut o;
#  ifdef SINGLE
  o.diffuse = g.diffuse * gbuffer_target.x + g.normal * gbuffer_target.y +
              g.depth * gbuffer_target.z + g.specular * gbuffer_target.w;
#  else
  o.diffuse = g.diffuse;
  o.normal = g.normal;
  o.depth = g.depth;
  o.specular = g.specular;
#  endif
  return o;
}
#endif
