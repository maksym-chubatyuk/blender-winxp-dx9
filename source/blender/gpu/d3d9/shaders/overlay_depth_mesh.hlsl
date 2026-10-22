/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Mesh depth for the overlays (`overlay_depth_mesh`), and the mesh surfaces of the selection
 * (`overlay_depth_mesh_selectable`, define SELECT).
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"
#include "select_common.hlsl"

#ifdef SELECT
// d3d9-resource-id 11 2
// d3d9-instance select_custom_id 11 8 4 instance u
float4 select_custom_id;
#else
// d3d9-resource-id 11 1
#endif
float4 view_buf[12];

struct VertIn {
  float3 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  SELECT_VARYING
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  o.position = gpu_position(drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0))));
#ifdef SELECT
  o.select_data = select_data_get(select_custom_id.x, o.position);
#endif
  return o;
}

float4 frag(VertOut v) : COLOR0
{
#ifdef SELECT
  return select_output(v.select_data);
#else
  return float4(0.0, 0.0, 0.0, 0.0);
#endif
}
