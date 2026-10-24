/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU shadow maps (`mechazyiu_shadow`, `mechazyiu_shadow_ortho` with define ORTHO): the surfaces seen
 * from a light, storing the normalized distance to it (orthographic: the depth), compared by
 * `mechazyiu_shadow_mask.hlsl`.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];

/* xyz: light position (world space), w: 1 / far distance. */
float4 shadow_light;

struct VertIn {
  float3 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
#ifdef ORTHO
  float depth : TEXCOORD0;
#else
  float3 to_light : TEXCOORD0;
#endif
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  float4 clip = drw_mat_mul(view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0)));
  o.position = gpu_position(clip);
#ifdef ORTHO
  o.depth = clip.z * 0.5 + 0.5;
#else
  o.to_light = (world_pos - shadow_light.xyz) * shadow_light.w;
#endif
  return o;
}

float4 frag(VertOut v) : COLOR0
{
#ifdef ORTHO
  return v.depth.xxxx;
#else
  return length(v.to_light).xxxx;
#endif
}
