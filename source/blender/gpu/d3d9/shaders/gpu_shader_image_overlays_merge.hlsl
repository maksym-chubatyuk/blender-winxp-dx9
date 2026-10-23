/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Viewport color and overlays merged without OCIO (`gpu_shader_2D_image_overlays_merge`): off-screen
 * viewport renders (viewport render, file thumbnails).
 */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
float display_transform;
float overlay;
float use_hdr_display;
sampler2D image_texture : register(s0);
sampler2D overlays_texture : register(s1);

struct VertIn {
  float2 pos : TEXCOORD0;
  float2 texCoord : TEXCOORD1;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  o.position = gpu_position(mul(ModelViewProjectionMatrix, float4(v.pos, 0.0, 1.0)));
  o.uv = v.texCoord;
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  float4 col = tex2D(image_texture, v.uv);
  float4 overlay_col = tex2D(overlays_texture, v.uv);
  if (overlay > 0.5) {
    /* With HDR, interpolate towards the clamped color for alpha-blended overlays. */
    col = use_hdr_display > 0.5 ? lerp(col, saturate(col), overlay_col.a) : col;
    col = col * (1.0 - overlay_col.a) + overlay_col;
  }
  if (display_transform > 0.5) {
    float3 lin = max(col.rgb, 0.0);
    float3 encoded = 1.055 * pow(lin, 1.0 / 2.4) - 0.055;
    col.rgb = (lin < 0.0031308) ? lin * 12.92 : encoded;
  }
  return col;
}
