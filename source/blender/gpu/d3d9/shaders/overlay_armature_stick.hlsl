/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Stick bones (`overlay_armature_stick`): screen aligned shapes between the bone ends.
 */

#include "overlay_armature_common.hlsl"

/* BoneStickData: bone_start, bone_end, wire_color, bone_color, head_color, tail_color. */
// d3d9-instance stick_data 0 96 0 instance ffffffffffffffffffffffff
float4 stick_data[6];
/* Theme sizes: pixel. */
float4 uniform_buf__94;

#define COL_WIRE 1.0
#define COL_HEAD 2.0
#define COL_TAIL 4.0
#define COL_BONE 8.0
#define POS_HEAD 16.0
#define POS_TAIL 32.0
#define POS_BONE 64.0

float flag(float flags, float bit)
{
  return fmod(floor(flags / bit), 2.0);
}

struct VertIn {
  /* Bone aligned screen space. */
  float2 pos : TEXCOORD0;
  float vclass : TEXCOORD1;
};

struct VertOut {
  float4 position : POSITION;
  float4 inner_color : TEXCOORD0;
  float4 wire_color : TEXCOORD1;
  float color_fac : TEXCOORD2;
  SELECT_VARYING
};

VertOut vert(VertIn v)
{
  VertOut o;
  float4 wire_color = stick_data[2];
  float4 inner_color = flag(v.vclass, COL_HEAD) > 0.5 ? stick_data[4] : stick_data[5];
  inner_color = flag(v.vclass, COL_BONE) > 0.5 ? stick_data[3] : inner_color;
  o.inner_color = inner_color;
  o.wire_color = wire_color.a > 0.0 ? wire_color : inner_color;
  o.color_fac = flag(v.vclass, COL_WIRE) > 0.5 ? 0.0 :
                                                 (flag(v.vclass, COL_BONE) > 0.5 ? 1.0 : 2.0);

  float3 v0 = world_to_view(stick_data[0].xyz);
  float3 v1 = world_to_view(stick_data[1].xyz);
  bool persp = is_persp();
  /* Clip the bone to the camera origin plane (not the clip plane) to avoid glitches if one end
   * is behind the camera origin (in perspective mode). */
  float clip_dist = persp ? -1e-7 : 1e20;
  float3 bvec = v1 - v0;
  float3 clip_pt = v0 + bvec * ((v0.z - clip_dist) / -bvec.z);
  if (v0.z > clip_dist) {
    v0 = clip_pt;
  }
  else if (v1.z > clip_dist) {
    v1 = clip_pt;
  }
  float4 p0 = view_to_clip(v0);
  float4 p1 = view_to_clip(v1);

  bool is_head = flag(v.vclass, POS_HEAD) > 0.5;
  bool is_bone = flag(v.vclass, POS_BONE) > 0.5;
  float h = is_head ? p0.w : p1.w;
  float2 x_screen_vec = normalize(clip_to_screen(p1) - clip_to_screen(p0) + 1e-8);
  float2 y_screen_vec = float2(x_screen_vec.y, -x_screen_vec.x);
  /* 2D screen aligned pos at the point. */
  float2 vpos = v.pos.x * x_screen_vec + v.pos.y * y_screen_vec;
  vpos *= persp ? h : 1.0;
  vpos *= wire_color.a > 0.0 ? 1.0 : 0.5;

  float4 hs_P = is_head ? p0 : p1;
  float stick_size = uniform_buf__94.x * 5.0;
  hs_P.xy += stick_size * (vpos * size_viewport_inv);
  /* Avoid Z fighting of head/tails. */
  hs_P.z += is_bone ? 0.0 : 1e-6;
  o.position = inner_color.a > 0.0 ? gpu_position(hs_P) : DISCARDED_POSITION;
#ifdef SELECT
  o.select_data = select_data_get(select_instance_id.x, o.position);
#endif
  return o;
}

FragOut frag(VertOut v)
{
  FragOut o;
  float fac = smoothstep(1.0, 0.2, v.color_fac);
  o.color = float4(lerp(v.inner_color.rgb, v.wire_color.rgb, fac), alpha);
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
#ifdef SELECT
  o.color = select_output(v.select_data);
#endif
  return o;
}
