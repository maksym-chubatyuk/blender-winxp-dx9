/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Image editor backdrop and image tiles (`overlay_grid_background`, `overlay_grid_image`): uniform
 * color quads. Defines: IMAGE (one quad per tile instance).
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];
float4 ucolor;
#ifdef IMAGE
/* tile_pos_buf, float3 elements (16 bytes in storage buffers). */
// d3d9-instance tile_pos 0 16 0 instance fff
float4 tile_pos;
#  define TILE_SCALE float3(1.0, 1.0, 1.0)
#  define TILE_POS tile_pos.xyz
#else
float3 tile_scale;
#  define TILE_SCALE tile_scale
/* The background is placed at z=1. */
#  define TILE_POS float3(0.0, 0.0, 1.0)
#endif

struct VertIn {
  float3 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
};

VertOut vert(VertIn v)
{
  VertOut o;
  /* `pos` is a quad (-1..1), the image plane is 0..1. */
  float3 image_pos = v.pos * 0.5 + 0.5;
  float3 world_pos = drw_point_object_to_world(TILE_SCALE * image_pos + TILE_POS);
  o.position = gpu_position(drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0))));
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  return ucolor;
}
