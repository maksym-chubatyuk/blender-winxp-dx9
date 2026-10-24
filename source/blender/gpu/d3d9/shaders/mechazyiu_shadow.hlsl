/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU shadow maps (`mechazyiu_shadow`, `mechazyiu_shadow_ortho` with define ORTHO): the
 * surfaces seen from a light, storing the normalized distance to it (orthographic: the depth) in
 * 24 bits (`mechazyiu_distance_encode`), compared by `mechazyiu_shadow_mask.hlsl`.
 *
 * CUTOUT: image alpha cutouts, alpha tested at 0.5 (TEXTURE: always with a texture).
 * TRANSMIT (`mechazyiu_shadow_transmit`): transparent casters, multiplied into the transmittance
 * map (the light they let through, 1 - alpha), TEXTURE: the alpha of their image counts.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"
#include "mechazyiu_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];

/* xyz: light position (world space), w: 1 / far distance. */
float4 shadow_light;

#if defined(CUTOUT) || defined(TRANSMIT)
float4 mat_base;
/* w: 1 when the alpha of the texture counts. */
float4 mat_params;
#endif
#ifdef CUTOUT
// d3d9-alpha-test
#endif
#ifdef TEXTURE
sampler2D base_tx : register(s0);
#endif

struct VertIn {
  float3 pos : TEXCOORD0;
#ifdef TEXTURE
  float2 au : TEXCOORD2;
#endif
};

struct VertOut {
  float4 position : POSITION;
#ifdef ORTHO
  float depth : TEXCOORD0;
#else
  float3 to_light : TEXCOORD0;
#endif
#ifdef TEXTURE
  float2 uv : TEXCOORD2;
#endif
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  float4 clip = drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0)));
  o.position = gpu_position(clip);
#ifdef ORTHO
  o.depth = clip.z * 0.5 + 0.5;
#else
  o.to_light = (world_pos - shadow_light.xyz) * shadow_light.w;
#endif
#ifdef TEXTURE
  o.uv = v.au;
#endif
  return o;
}

float4 frag(VertOut v) : COLOR0
{
#ifdef TRANSMIT
  float alpha = mat_base.a;
#  ifdef TEXTURE
  alpha *= lerp(1.0, tex2D(base_tx, v.uv).a, mat_params.w);
#  endif
  return float4((1.0 - alpha).xxx, 1.0);
#else
#  ifdef ORTHO
  float4 result = float4(mechazyiu_distance_encode(v.depth), 1.0);
#  else
  float4 result = float4(mechazyiu_distance_encode(length(v.to_light)), 1.0);
#  endif
#  ifdef CUTOUT
  result.a = tex2D(base_tx, v.uv).a;
#  endif
  return result;
#endif
}
