/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Display transform of the viewport and images (`OCIO_Display`, generated from the OpenColorIO
 * configuration for the other backends).
 *
 * The "Standard" view of sRGB displays is exact: scene linear Rec.709 to sRGB, with exposure,
 * gamma and the merge of the overlays. Other views and looks (D3D9_VIEW_LUT,
 * `OCIO_Display_lut`) go through a lattice made on the CPU: 32^3 colors over log2 of the scene
 * linear color, holding the square root of the display linear color (see
 * `libocio_gpu_shader_binder.cc`). Curve mapping is ignored.
 */

#include "d3d9_common.hlsl"

/* OCIO_GPUParameters: dither, exponent, opacity, use_predivide, do_overlay_merge,
 * use_hdr_display, 2 pads, scene_linear_matrix. */
// d3d9-ubo parameters fffb bbii ffff ffff ffff ffff
float4 parameters[6];
float4x4 ModelViewProjectionMatrix;
sampler2D image_texture : register(s0);
sampler2D overlay_texture : register(s1);
#ifdef D3D9_VIEW_LUT
sampler3D view_lut : register(s3);
#endif

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
  float3 lin = max(col.rgb, 0.0);
#ifdef D3D9_VIEW_LUT
  /* Lattice coordinates: log2 from -12.47393 over 25 stops, texel centers (32 texels). The first
   * texel is black. */
  float3 position = saturate(log2(max(lin, 1e-10)) * (1.0 / 25.0) + (12.47393 / 25.0));
  float3 view = tex3D(view_lut, position * (31.0 / 32.0) + (0.5 / 32.0)).rgb;
  lin = view * view;
#endif

  /* The display gamma (`exponent`) and the overlay merge are done in linear space, then the
   * gamma and the sRGB encoding are a single power (the 64 instructions of ps_2_0 are slow over
   * the viewport on R300 hardware). The overlays are stored linear (render targets aren't
   * encoded) and pre-multiplied. */
  float merge = do_overlay_merge > 0.5 ? 1.0 : 0.0;
  float3 merged = lin * (1.0 - overlay.a) + overlay.rgb;
  lin = lerp(lin, merged, merge);
  float3 encoded = 1.055 * pow(lin, exponent / 2.4) - 0.055;
  col.rgb = (lin < 0.0031308) ? lin * 12.92 : encoded;
  col.a = lerp(col.a, col.a * (1.0 - overlay.a) + overlay.a, merge);
  col.a *= opacity;
  return col;
}
