/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** Screen space patterns. Defines: CHECKER, STRIPES. */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
float4 color1;
float4 color2;
#ifdef CHECKER
float size;
#else
float size1;
float size2;
#endif

struct VertIn {
  float2 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  float2 frag_co : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float4 clip_pos = mul(ModelViewProjectionMatrix, float4(v.pos, 0.0, 1.0));
  o.position = gpu_position(clip_pos);
  o.frag_co = gpu_frag_coord(clip_pos);
  return o;
}

float4 frag(VertOut v) : COLOR0
{
#ifdef CHECKER
  float2 phase = fmod(v.frag_co, size * 2.0);
  return ((phase.x > size) != (phase.y > size)) ? color1 : color2;
#else
  float phase = fmod(v.frag_co.x + v.frag_co.y, size1 + size2);
  return (phase < size1) ? color1 : color2;
#endif
}
