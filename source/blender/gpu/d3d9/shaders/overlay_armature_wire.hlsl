/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Wire bones (`overlay_armature_wire`): lines read from the vertex storage buffer. No selectable
 * variant (the select ids are per line).
 */

#include "overlay_armature_common.hlsl"

/* VertexData: pos_, color_. */
// d3d9-vertex-pull 0 32 0 ff
struct VertIn {
  float4 pos : TEXCOORD0;
  float4 color : TEXCOORD1;
};

struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  o.color = float4(v.color.rgb, alpha);
  o.position = gpu_position(view_to_clip(world_to_view(v.pos.xyz)));
  return o;
}

FragOut frag(VertOut v)
{
  FragOut o;
  o.color = v.color;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
  return o;
}
