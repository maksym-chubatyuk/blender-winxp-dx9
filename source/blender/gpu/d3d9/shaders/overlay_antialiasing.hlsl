/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Overlay resolve (`overlay_antialiasing.bsl.hh`). Without depth sampling and with 1 pixel lines,
 * the line anti-aliasing is skipped: the overlay color is passed through.
 */

#include "d3d9_common.hlsl"
#define DRW_NO_MODEL
#include "draw_common.hlsl"

sampler2D color_tx : register(s1);

struct VertIn {
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float2 pos = drw_fullscreen_pos(v.vertex_id);
  o.position = gpu_position(float4(pos, 1.0, 1.0));
  o.uv = pos * 0.5 + 0.5;
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  return tex2D(color_tx, v.uv);
}
