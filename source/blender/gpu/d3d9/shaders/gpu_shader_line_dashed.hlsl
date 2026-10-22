/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Dashed lines, see `gpu_shader_2D_line_dashed_frag.glsl`.
 *
 * Without flat interpolation the line start can't be passed to the fragment shader: dashes are
 * computed from the screen position along the dominant axis of the line instead.
 */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
float2 viewport_size;
float dash_width;
float udash_factor;
float colors_len;
float4 color;
float4 color2;

struct VertIn {
  float3 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  float2 screen_pos : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float4 clip_pos = mul(ModelViewProjectionMatrix, float4(v.pos, 1.0));
  o.position = gpu_position(clip_pos);
  o.screen_pos = viewport_size * 0.5 * (clip_pos.xy / clip_pos.w);
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  if (udash_factor >= 1.0) {
    return color;
  }
  float distance_along_line = v.screen_pos.x + v.screen_pos.y;
  float normalized_distance = frac(distance_along_line / dash_width);
  if (normalized_distance <= udash_factor) {
    return color;
  }
  clip(colors_len - 0.5);
  return color2;
}
