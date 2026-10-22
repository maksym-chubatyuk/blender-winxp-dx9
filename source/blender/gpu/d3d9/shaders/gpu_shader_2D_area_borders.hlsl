/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** Rounded corners between editors, see `gpu_shader_2D_area_borders_vert.glsl`. */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
float4 rect;
float4 color;
float scale;
float width;
float cornerLen;

struct VertIn {
  float2 pos : TEXCOORD0;
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  float corner_id = fmod(floor((v.vertex_id + 0.5) / cornerLen), 4.0);
  bool inner = all(abs(v.pos) < 1.0);
  float2 final_pos = v.pos * (inner ? (1.0 - width) : 1.05);

  VertOut o;
  o.uv = final_pos;
  if (corner_id < 0.5) {
    final_pos = (final_pos - float2(1.0, 1.0)) * scale + rect.yw;
  }
  else if (corner_id < 1.5) {
    final_pos = (final_pos - float2(-1.0, 1.0)) * scale + rect.xw;
  }
  else if (corner_id < 2.5) {
    final_pos = (final_pos - float2(-1.0, -1.0)) * scale + rect.xz;
  }
  else {
    final_pos = (final_pos - float2(1.0, -1.0)) * scale + rect.yz;
  }
  o.position = gpu_position(mul(ModelViewProjectionMatrix, float4(final_pos, 0.0, 1.0)));
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  float dist = (length(v.uv) - (0.98 - width)) * scale;
  float4 c = color;
  c.a *= smoothstep(-0.09, 1.09, dist);
  return blender_srgb_to_framebuffer_space(c);
}
