/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Light ground lines and their circle on the ground (`overlay_extra_groundline`). The light
 * positions are instance data (`data_buf`).
 */

#include "d3d9_common.hlsl"
#define DRW_NO_MODEL
#include "draw_common.hlsl"
#include "select_common.hlsl"

#ifdef SELECT
// d3d9-instance select_instance_id 5 4 0 instance u
float4 select_instance_id;
#endif

float4 view_buf[12];
/* Light position of the instance. */
// d3d9-instance light_pos 0 16 0 instance ffff
float4 light_pos;
/* Theme colors: light. */
float4 uniform_buf__7;
/* Theme sizes: pixel. */
float4 uniform_buf__94;
/* fresnel_mix_edit, pixel_fac. */
float4 uniform_buf__98;

struct VertIn {
  float3 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
  SELECT_VARYING
};

VertOut vert(VertIn v)
{
  VertOut o;
  /* Relative to DPI scaling. Have constant screen size. */
  float3 screen_pos = view_buf[DRW_VIEWINV].xyz * v.pos.x + view_buf[DRW_VIEWINV + 1].xyz * v.pos.y;
  float3 p = light_pos.xyz;
  p.z *= (v.pos.z == 0.0) ? 0.0 : 1.0;
  /* `mul_project_m4_v3_zfac`: the W of the projected point. */
  float zfac = drw_mat_mul(view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(p, 1.0)))
                   .w;
  float screen_size = uniform_buf__98.y * zfac * uniform_buf__94.x;
  float3 world_pos = p + screen_pos * screen_size;
  float4 clip_pos = drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0)));
  o.position = gpu_position(clip_pos);
  o.color = uniform_buf__7;
#ifdef SELECT
  o.select_data = select_data_get(select_instance_id.x, o.position);
#endif
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

FragOut frag(VertOut v)
{
  FragOut o;
  o.color = v.color;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
#ifdef SELECT
  o.color = select_output(v.select_data);
#endif
  return o;
}
