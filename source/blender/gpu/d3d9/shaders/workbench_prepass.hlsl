/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Workbench opaque mesh pre-pass (`workbench_prepass.bsl.hh`): fills the G-buffer.
 *
 * D3D9 differences: the material buffer is 8 bit (multiple render targets need the same bit
 * depth), so the packed roughness/metallic pair is stored divided by 255. No back-face normal
 * flipping (ps_2_0 has no `VFACE`).
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

// d3d9-resource-id 11 2
float4 view_buf[12];

/* Material of the custom id (`materials_data`, WB_MATERIAL_SLOT): color, then the packed
 * alpha / roughness / metallic bytes: (r, g, b, metallic) (roughness, alpha, -, -). */
// d3d9-instance wb_material 0 16 0 custom fffB
float4 wb_material[2];
/* Resource id of the instance, for the object id. */
// d3d9-instance wb_resource_id 11 8 0 instance u
float4 wb_resource_id;

struct VertIn {
  float3 pos : TEXCOORD0;
  float3 nor : TEXCOORD1;
  float4 ac : TEXCOORD2;
};

struct VertOut {
  float4 position : POSITION;
  float3 normal : TEXCOORD0;
  float3 color : TEXCOORD1;
  float3 data : TEXCOORD2; /* roughness, metallic, object id */
};

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  float4 view_pos = drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0));
  o.position = gpu_position(drw_mat_mul(view_buf, DRW_WINMAT, view_pos));
  o.normal = drw_mat_mul3(view_buf, DRW_VIEWMAT, drw_normal_object_to_world(v.nor));
  o.color = (wb_material[0].r == -1.0) ? v.ac.rgb : wb_material[0].rgb;
  o.data = float3(wb_material[1].x, wb_material[0].w, fmod(wb_resource_id.x, 65536.0) + 1.0);
  return o;
}

struct FragOut {
  float4 material : COLOR0;
  float4 normal : COLOR1;
  float4 object_id : COLOR2;
};

FragOut frag(VertOut v)
{
  FragOut o;
  float3 n = normalize(v.normal);
  float p = sqrt(n.z * 8.0 + 8.0);
  o.normal = float4(saturate(n.xy / p + 0.5), 0.0, 0.0);
  float pair = floor(v.data.x * 31.0) + floor(v.data.y * 7.0) * 32.0;
  o.material = float4(v.color, pair / 255.0);
  o.object_id = v.data.zzzz;
  return o;
}
