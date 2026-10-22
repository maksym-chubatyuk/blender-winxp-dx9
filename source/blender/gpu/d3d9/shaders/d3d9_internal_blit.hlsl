/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** Backend internal: copy a texture to the bound target (blits that flip the image). */

sampler2D image : register(s0);

struct VertIn {
  float2 pos : TEXCOORD0;
  float2 uv : TEXCOORD1;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  o.position = float4(v.pos, 0.0, 1.0);
  o.uv = v.uv;
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  return tex2D(image, v.uv);
}
