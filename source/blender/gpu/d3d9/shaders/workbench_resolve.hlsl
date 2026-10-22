/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Workbench opaque resolve (`workbench_composite.bsl.hh`): lighting from the G-buffer.
 * Defines: FLAT (no lighting).
 *
 * D3D9 differences: the depth can't be sampled, the background is skipped by the depth test
 * instead (the triangle is drawn on the far plane, see `workbench_mesh_passes.cc`).
 * Specular lighting, cavity, curvature and shadows are not supported.
 */

#include "d3d9_common.hlsl"
#define DRW_NO_MODEL
#include "draw_common.hlsl"

/* WorldData, the integers are cavity_sample_start/end, matcap_orientation, use_specular. */
// d3d9-ubo world_data ffff ffff ffff ffff ffff ffff ffff ffff ffff ffff ffff ffff ffff ffff ffff ffff ffff iiff ffff ffff ibfi ffff
float4 world_data__4[13]; /* lights[4] (direction, specular, diffuse + wrap), ambient. */
sampler2D normal_tx : register(s4);
sampler2D material_tx : register(s5);

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

#define LIGHT_DIR(i) world_data__4[(i) * 3].xyz
#define LIGHT_SPEC(i) world_data__4[(i) * 3 + 1].rgb
#define LIGHT_DIFF(i) world_data__4[(i) * 3 + 2]
#define AMBIENT world_data__4[12].rgb

float4 wrapped_lighting(float4 NL, float4 w)
{
  float4 w_1 = w + 1.0;
  return saturate((NL + w) / (w_1 * w_1));
}

float4 frag(VertOut v) : COLOR0
{
  float4 mat = tex2D(material_tx, v.uv);
  float3 base_color = mat.rgb;
#ifdef FLAT
  return float4(base_color, 1.0);
#else
  float2 fenc = tex2D(normal_tx, v.uv).xy * 4.0 - 2.0;
  float f = dot(fenc, fenc);
  float3 N = float3(fenc * sqrt(1.0 - f / 4.0), 1.0 - f / 2.0);

  float4 wrap = float4(LIGHT_DIFF(0).a, LIGHT_DIFF(1).a, LIGHT_DIFF(2).a, LIGHT_DIFF(3).a);
  float4 diff_NL = float4(
      dot(LIGHT_DIR(0), N), dot(LIGHT_DIR(1), N), dot(LIGHT_DIR(2), N), dot(LIGHT_DIR(3), N));
  float4 diff_light = wrapped_lighting(diff_NL, wrap);
  float3 diffuse_light = AMBIENT + diff_light.x * LIGHT_DIFF(0).rgb +
                         diff_light.y * LIGHT_DIFF(1).rgb + diff_light.z * LIGHT_DIFF(2).rgb +
                         diff_light.w * LIGHT_DIFF(3).rgb;
  /* Specular lighting doesn't fit the 64 instructions of ps_2_0. */
  return float4(diffuse_light * base_color, 1.0);
#endif
}
