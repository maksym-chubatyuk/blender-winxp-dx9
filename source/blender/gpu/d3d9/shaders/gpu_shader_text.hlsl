/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Text: glyph quads from the 2D glyph atlas BLF uses without storage buffers (`blf_font.cc`,
 * #blf_batch_draw_atlas), instead of the instanced buffer texture glyphs of `gpu_shader_text`.
 *
 * Shadows: 3x3 blur, 5x5 blur (approximated by a 3x3 kernel with 2 texel spacing) and outline
 * (3x3 dilation).
 */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
/* Atlas width and height (the uniforms of the buffer texture layout of the GLSL version). */
float glyph_tex_width_mask;
float glyph_tex_width_shift;
sampler2D glyph : register(s0);

struct VertIn {
  float2 pos : TEXCOORD0;
  float4 glyph : TEXCOORD1; /* uv, shadow type */
  float4 color : TEXCOORD2;
};

struct VertOut {
  float4 position : POSITION;
  float3 uv_type : TEXCOORD0;
  float4 color : TEXCOORD1;
  float2 texel : TEXCOORD2;
};

VertOut vert(VertIn v)
{
  VertOut o;
  o.position = gpu_position(mul(ModelViewProjectionMatrix, float4(v.pos, 0.0, 1.0)));
  o.uv_type = v.glyph.xyz;
  o.color = v.color;
  float spacing = (v.glyph.z > 4.5 && v.glyph.z < 5.5) ? 2.0 : 1.0;
  o.texel = spacing / float2(glyph_tex_width_mask, glyph_tex_width_shift);
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  float2 uv = v.uv_type.xy;
  float type = v.uv_type.z;
  float2 d = v.texel;
  float c = tex2D(glyph, uv).r;
  float l = tex2D(glyph, uv - float2(d.x, 0.0)).r;
  float r = tex2D(glyph, uv + float2(d.x, 0.0)).r;
  float t = tex2D(glyph, uv - float2(0.0, d.y)).r;
  float b = tex2D(glyph, uv + float2(0.0, d.y)).r;
  float tl = tex2D(glyph, uv - d).r;
  float br = tex2D(glyph, uv + d).r;
  float tr = tex2D(glyph, uv + float2(d.x, -d.y)).r;
  float bl = tex2D(glyph, uv + float2(-d.x, d.y)).r;

  float blur = (4.0 * c + 2.0 * (l + r + t + b) + (tl + tr + bl + br)) * (1.0 / 16.0);
  float outline = max(max(max(c, l), max(r, t)), max(max(b, tl), max(max(br, tr), bl)));
  float alpha = (type < 0.5) ? c : ((type > 5.5) ? outline : blur);
  return float4(v.color.rgb, alpha * v.color.a);
}
