/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Textured shaders. Defines: USE_COLOR (multiply by `color`), DESATURATE, SCENE_LINEAR (convert from scene
 * linear to sRGB display).
 */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
#ifdef USE_COLOR
float4 color;
#endif
#ifdef DESATURATE
float factor;
#endif
sampler2D image : register(s0);

struct VertIn {
  float3 pos : TEXCOORD0;
  float2 texCoord : TEXCOORD1;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  o.position = gpu_position(mul(ModelViewProjectionMatrix, float4(v.pos, 1.0)));
  o.uv = v.texCoord;
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  float4 c = tex2D(image, v.uv);
#ifdef DESATURATE
  c.rgb = (0.3333333 * factor) * (c.r + c.g + c.b) + c.rgb * (1.0 - factor);
#endif
#ifdef USE_COLOR
  c *= color;
#endif
#ifdef SCENE_LINEAR
  /* Rec.709 linear to sRGB, approximated with a gamma curve. */
  c.rgb = pow(max(c.rgb, 0.0), 1.0 / 2.2);
#endif
  return c;
}
