/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Uniform, flat and smooth color shaders, including the poly-line variants (drawn as 1 pixel wide
 * lines). Defines: UNIFORM_COLOR, POINT_SIZE.
 */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
#ifdef UNIFORM_COLOR
float4 color;
#endif
#ifdef POINT_SIZE
float size;
#endif

struct VertIn {
  float3 pos : TEXCOORD0;
#ifndef UNIFORM_COLOR
  float4 color : TEXCOORD1;
#endif
};

struct VertOut {
  float4 position : POSITION;
#ifndef UNIFORM_COLOR
  float4 color : TEXCOORD0;
#endif
#ifdef POINT_SIZE
  float point_size : PSIZE;
#endif
};

VertOut vert(VertIn v)
{
  VertOut o;
  o.position = gpu_position(mul(ModelViewProjectionMatrix, float4(v.pos, 1.0)));
#ifndef UNIFORM_COLOR
  o.color = v.color;
#endif
#ifdef POINT_SIZE
  o.point_size = size;
#endif
  return o;
}

float4 frag(VertOut v) : COLOR0
{
#ifdef UNIFORM_COLOR
  return color;
#else
  return v.color;
#endif
}
