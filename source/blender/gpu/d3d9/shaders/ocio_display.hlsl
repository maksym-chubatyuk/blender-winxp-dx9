/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Display transform of the viewport and images (`OCIO_Display`, generated from the OpenColorIO
 * configuration for the other backends).
 *
 * Only the "Standard" view of sRGB displays is implemented: scene linear Rec.709 to sRGB, with
 * exposure, gamma and the merge of the overlays. Curve mapping and looks are ignored.
 */

#include "d3d9_common.hlsl"

/* OCIO_GPUParameters: dither, exponent, opacity, use_predivide, do_overlay_merge,
 * use_hdr_display, 2 pads, scene_linear_matrix. */
// d3d9-ubo parameters fffb bbii ffff ffff ffff ffff
float4 parameters[6];
float4x4 ModelViewProjectionMatrix;
sampler2D image_texture : register(s0);
sampler2D overlay_texture : register(s1);

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

float3 linear_to_srgb(float3 c)
{
  c = max(c, 0.0);
  float3 lo = c * 12.92;
  float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
  return (c < 0.0031308) ? lo : hi;
}

float4 frag(VertOut v) : COLOR0
{
  float4 col = tex2D(image_texture, v.uv);
  float4 overlay = tex2D(overlay_texture, v.uv);
  float exponent = parameters[0].y;
  float opacity = parameters[0].z;
  float use_predivide = parameters[0].w;
  float do_overlay_merge = parameters[1].x;

  if (use_predivide > 0.5 && col.a > 0.0 && col.a < 1.0) {
    col.rgb /= col.a;
  }
  /* Exposure and white balance. The matrix columns are the rows of the HLSL constructor. */
  float4x4 scene_linear_matrix = float4x4(parameters[2], parameters[3], parameters[4], parameters[5]);
  col = mul(col, scene_linear_matrix);

  col.rgb = linear_to_srgb(col.rgb);

  if (do_overlay_merge > 0.5) {
    col.rgb = pow(abs(col.rgb), exponent * 2.2);
    col *= 1.0 - overlay.a;
    col += overlay;
    col.rgb = pow(abs(col.rgb), 1.0 / 2.2);
  }
  else {
    col.rgb = pow(abs(col.rgb), exponent);
  }
  col.a *= opacity;
  return col;
}
