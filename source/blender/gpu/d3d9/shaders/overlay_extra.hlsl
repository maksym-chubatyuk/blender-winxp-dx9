/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Object extras: cameras, lights, empties (`overlay_extra_vert.glsl`).
 * Not supported: spot cone silhouettes, stereo camera volumes, camera distance colors.
 */

#include "d3d9_common.hlsl"
#define DRW_NO_MODEL
#include "draw_common.hlsl"
#include "select_common.hlsl"

#ifdef SELECT
// d3d9-instance select_instance_id 5 4 0 instance u
float4 select_instance_id;
#endif

float4 view_buf[16];
/* ExtraInstanceData of the instance: color, object_to_world (with packed data in the last
 * row). */
// d3d9-instance extra_data 0 80 0 instance ffffffffffffffffffff
float4 extra_data[5];
/* fresnel_mix_edit, pixel_fac. */
float4 uniform_buf__98;
/* Theme sizes: pixel. */
float4 uniform_buf__94;

#define VCLASS_LIGHT_AREA_SHAPE 1.0
#define VCLASS_LIGHT_SPOT_SHAPE 2.0
#define VCLASS_LIGHT_SPOT_BLEND 4.0
#define VCLASS_LIGHT_DIST 16.0
#define VCLASS_CAMERA_FRAME 32.0
#define VCLASS_CAMERA_DIST 64.0
#define VCLASS_CAMERA_VOLUME 128.0
#define VCLASS_SCREENSPACE 256.0
#define VCLASS_SCREENALIGNED 512.0
#define VCLASS_EMPTY_SCALED 1024.0
#define VCLASS_EMPTY_AXES 2048.0
#define VCLASS_EMPTY_SIZE 16384.0

float flag(float flags, float bit)
{
  return fmod(floor(flags / bit), 2.0);
}

struct VertIn {
  float3 pos : TEXCOORD0;
  float vclass : TEXCOORD1;
};

struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
  SELECT_VARYING
};

VertOut vert(VertIn v)
{
  VertOut o;
  float4 color = extra_data[0];
  float4 inst_data = float4(extra_data[1].w, extra_data[2].w, extra_data[3].w, extra_data[4].w);
  float3 ob_x = extra_data[1].xyz;
  float3 ob_y = extra_data[2].xyz;
  float3 ob_z = extra_data[3].xyz;
  float3 ob_loc = extra_data[4].xyz;
  float vclass = v.vclass;

  float4 final_color = color;
  final_color.a = color.a < 0.0 ? 1.0 : color.a;

  float3 vpos = v.pos;
  float3 vofs = float3(0.0, 0.0, 0.0);
  if (flag(vclass, VCLASS_LIGHT_AREA_SHAPE) > 0.5) {
    float2 area_size = color.a < 0.0 ? -color.aa : inst_data.xy;
    vpos.xy *= area_size;
  }
  else if (flag(vclass, VCLASS_LIGHT_SPOT_SHAPE) > 0.5) {
    float spot_sine = sqrt(1.0 - inst_data.x * inst_data.x);
    spot_sine *= flag(vclass, VCLASS_LIGHT_SPOT_BLEND) > 0.5 ? inst_data.y : 1.0;
    vpos = float3(v.pos.xy * spot_sine, -inst_data.x);
  }
  else if (flag(vclass, VCLASS_CAMERA_FRAME) > 0.5) {
    float camera_dist = color.a;
    vpos.z = camera_dist > 0.0 ? -abs(camera_dist) : vpos.z * -abs(camera_dist);
    vpos.xy = (inst_data.zw + inst_data.xy * vpos.xy) * abs(vpos.z);
  }
  else if (flag(vclass, VCLASS_CAMERA_DIST) > 0.5) {
    vofs.z = -lerp(inst_data.z, inst_data.w, v.pos.z);
    vpos = float3(0.0, 0.0, 0.0);
  }
  else if (flag(vclass, VCLASS_EMPTY_SCALED) > 0.5) {
    vpos *= inst_data.w;
  }
  else if (flag(vclass, VCLASS_EMPTY_SIZE) > 0.5) {
    vpos *= inst_data.xyz;
  }
  else if (flag(vclass, VCLASS_EMPTY_AXES) > 0.5) {
    float axis = floor(vpos.z);
    float3 axis_mask = float3(axis == 0.0, axis == 1.0, axis == 2.0);
    vofs = axis_mask * (1.0 + frac(vpos.z)) * inst_data.w;
    float3 axis_vec = ob_x * axis_mask.x + ob_y * axis_mask.y + ob_z * axis_mask.z;
    vpos *= length(axis_vec) * inst_data.w;
    final_color.rgb = lerp(axis_mask + frac(v.pos.z), color.rgb, color.a);
    final_color.a = 1.0;
  }

  float3 world_pos;
  float3 p = ob_x * vofs.x + ob_y * vofs.y + ob_z * vofs.z + ob_loc;
  float3 screen_pos = view_buf[DRW_VIEWINV].xyz * vpos.x + view_buf[DRW_VIEWINV + 1].xyz * vpos.y;
  if (flag(vclass, VCLASS_SCREENSPACE) > 0.5) {
    /* Constant screen size. */
    float4 clip_p = drw_mat_mul(view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(p, 1.0)));
    world_pos = p + screen_pos * (uniform_buf__98.y * clip_p.w * uniform_buf__94.x);
  }
  else if (flag(vclass, VCLASS_SCREENALIGNED) > 0.5) {
    world_pos = p + screen_pos;
  }
  else {
    float3 q = vofs + vpos;
    world_pos = ob_x * q.x + ob_y * q.y + ob_z * q.z + ob_loc;
  }

  o.position = gpu_position(drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0))));
  o.color = final_color;
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
