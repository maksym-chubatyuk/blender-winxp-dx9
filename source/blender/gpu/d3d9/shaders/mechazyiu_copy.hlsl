/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU rectangle copy (`mechazyiu_copy`), and the viewport output (`mechazyiu_output`, define OUTPUT):
 * the running mean and the depth of the G-buffer for the overlays.
 */

#include "d3d9_common.hlsl"
#define MECHAZYIU_SCREEN_PASS
#include "mechazyiu_common.hlsl"

sampler2D source_tx : register(s0);
float4 uv_transform;

#ifdef OUTPUT
sampler2D depth_tx : register(s1);
float4 depth_proj;

struct FragOut {
  float4 color : COLOR0;
  float depth : DEPTH;
};

FragOut frag(VertOut v)
{
  FragOut o;
  o.color = tex2D(source_tx, v.uv);
  float depth = tex2D(depth_tx, v.uv * uv_transform.xy + uv_transform.zw).x;
  /* Linear depth to the depth buffer of the (not jittered) viewport projection. */
  float persp = depth_proj.z;
  float ndc = persp > 0.5 ? depth_proj.y / max(depth, 1e-8) - depth_proj.x :
                            depth_proj.y - depth_proj.x * depth;
  o.depth = depth > 0.0 ? saturate(ndc * 0.5 + 0.5) : 1.0;
  return o;
}
#else
float4 frag(VertOut v) : COLOR0
{
  return tex2D(source_tx, v.uv * uv_transform.xy + uv_transform.zw);
}
#endif
