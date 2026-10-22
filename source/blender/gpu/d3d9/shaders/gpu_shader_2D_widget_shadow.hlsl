/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** UI widget shadow, see `gpu_shader_2D_widget_shadow_vert.glsl`. */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
float4 parameters[4];
float alpha;

struct VertIn {
  float vflag : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  float2 shadow : TEXCOORD0; /* shadowFalloff, innerMask */
};

static const float2 cornervec[36] = {
    float2(0.0, 1.0),     float2(0.02, 0.805),   float2(0.067, 0.617),  float2(0.169, 0.45),
    float2(0.293, 0.293), float2(0.45, 0.169),   float2(0.617, 0.076),  float2(0.805, 0.02),
    float2(1.0, 0.0),     float2(-1.0, 0.0),     float2(-0.805, 0.02),  float2(-0.617, 0.067),
    float2(-0.45, 0.169), float2(-0.293, 0.293), float2(-0.169, 0.45),  float2(-0.076, 0.617),
    float2(-0.02, 0.805), float2(0.0, 1.0),      float2(0.0, -1.0),     float2(-0.02, -0.805),
    float2(-0.067, -0.617), float2(-0.169, -0.45), float2(-0.293, -0.293), float2(-0.45, -0.169),
    float2(-0.617, -0.076), float2(-0.805, -0.02), float2(-1.0, 0.0),   float2(1.0, 0.0),
    float2(0.805, -0.02), float2(0.617, -0.067), float2(0.45, -0.169),  float2(0.293, -0.293),
    float2(0.169, -0.45), float2(0.076, -0.617), float2(0.02, -0.805),  float2(0.0, -1.0)};

static const float2 center_offset[4] = {
    float2(1.0, 1.0), float2(-1.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0)};

VertOut vert(VertIn v)
{
  float4 recti = parameters[0];
  float4 rect = parameters[1];
  float radsi = parameters[2].x;
  float rads = parameters[2].y;
  float4 round_corners = parameters[3];

  /* Decode the flags: 2 bits corner, 4 bits corner vertex, bit 10 inner. */
  float flag = v.vflag + 0.5;
  float cflag = floor(fmod(flag, 4.0));
  float vofs = floor(fmod(flag / 4.0, 16.0));
  bool is_inner = fmod(flag / 1024.0, 2.0) >= 1.0;
  int corner = int(cflag);

  float shadow_width = rads - radsi;
  float shadow_width_top = rect.w - recti.w;
  float4 corner_mask = float4(cflag == 0.0, cflag == 1.0, cflag == 2.0, cflag == 3.0);
  float rad_inner = radsi * dot(round_corners, corner_mask);
  float rad_outer = rad_inner + shadow_width;
  float radius = is_inner ? rad_inner : rad_outer;
  bool top = cflag > 1.5;
  float shadow_offset = (is_inner && top) ? (shadow_width - shadow_width_top) : 0.0;

  float2 c = center_offset[corner];
  float2 center_outer = rad_outer * c;
  float2 center = radius * c;

  float2 p = rad_outer * cornervec[int(cflag * 9.0 + vofs)];
  if (is_inner && top && (p.y < (shadow_offset - rad_outer))) {
    p.y += shadow_width_top;
    p.x = 0.0;
  }
  else {
    p = radius * normalize(p - (center_outer + float2(0.0, shadow_offset))) + center;
  }

  float4 rct = is_inner ? recti : rect;
  p += float2(dot(corner_mask, float4(rct.x, rct.y, rct.y, rct.x)),
              dot(corner_mask, float4(rct.z, rct.z, rct.w, rct.w)));

  VertOut o;
  float inner_shadow_strength = min((rect.w - p.y) / rad_outer + 0.1, 1.0);
  o.shadow = float2(is_inner ? inner_shadow_strength : 0.0, is_inner ? 0.0 : 1.0);
  o.position = gpu_position(mul(ModelViewProjectionMatrix, float4(p, 0.0, 1.0)));
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  float falloff = v.shadow.x;
  float shadow_alpha = alpha * (falloff * falloff * 0.722 + falloff * 0.277);
  float inner_alpha = smoothstep(0.0, 0.05, v.shadow.y);
  return float4(0.0, 0.0, 0.0, inner_alpha * shadow_alpha); /* Black: same in any space. */
}
