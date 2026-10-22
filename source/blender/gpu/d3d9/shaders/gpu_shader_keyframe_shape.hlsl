/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Keyframe shapes of the animation editors (`gpu_shader_keyframe_shape`). The points are drawn as
 * quads: the shape parameters don't fit the 2 color registers left by point sprites.
 */

#include "d3d9_common.hlsl"

// d3d9-point-quads
float4x4 ModelViewProjectionMatrix;
float2 ViewportSize;
float outline_scale;

#define SHAPE_DIAMOND 1.0
#define SHAPE_CIRCLE 2.0
#define SHAPE_CLIPPED_VERTICAL 4.0
#define SHAPE_CLIPPED_HORIZONTAL 8.0
#define SHAPE_INNER_DOT 16.0
#define SHAPE_ARROW_END_MAX 256.0
#define SHAPE_ARROW_END_MIN 512.0
#define SHAPE_ARROW_END_MIXED 1024.0

#define LINE_FALLOFF 1.0
#define CIRCLE_SCALE 0.79788456 /* sqrt(2 / pi) */
#define SQUARE_SCALE 0.70710678
#define DIAGONAL_SCALE 0.70710678
#define MINMAX_BIAS 0.7
#define MINMAX_SCALE 0.64168895 /* sqrt(1 / (1 + 1 / minmax_bias)) */

float flag(float flags, float bit)
{
  return fmod(floor(flags / bit), 2.0);
}

struct VertIn {
  float4 color : TEXCOORD0;
  float4 outline_color : TEXCOORD1;
  float2 pos : TEXCOORD2;
  float size : TEXCOORD3;
  float flags : TEXCOORD4;
  float2 corner : TEXCOORD14;
};

struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
  float4 outline_color : TEXCOORD1;
  float4 radii : TEXCOORD2;
  float4 thresholds : TEXCOORD3;
  /* Point coordinate (`gl_PointCoord`, 0 to 1 with Y down). */
  float2 coord : TEXCOORD4;
  /* Flags as 0 or 1: diamond, circle, clipped vertical, clipped horizontal. */
  float4 shape : TEXCOORD5;
  /* Inner dot, arrow max, arrow min, arrow alpha (mixed or not). */
  float4 extra : TEXCOORD6;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float4 pos = mul(ModelViewProjectionMatrix, float4(v.pos, 0.0, 1.0));
  /* Align to pixel grid if the viewport size is known. */
  if (ViewportSize.x > 0.0) {
    float2 scale = ViewportSize * 0.5;
    float2 px_pos = (pos.xy + 1.0) * scale;
    float2 adj_pos = floor(px_pos - 0.5 + 0.5) + 0.5;
    pos.xy = adj_pos / scale - 1.0;
  }

  float flags = v.flags;
  float shapes = flag(flags, SHAPE_DIAMOND) + flag(flags, SHAPE_CIRCLE) +
                 flag(flags, SHAPE_CLIPPED_VERTICAL) + flag(flags, SHAPE_CLIPPED_HORIZONTAL);
  flags += shapes > 0.5 ? 0.0 : SHAPE_DIAMOND;

  /* Size-dependent line thickness. */
  float half_width = 0.06 + (v.size - 10.0) * 0.04;
  float line_width = half_width + LINE_FALLOFF;
  float4 thresholds;
  thresholds.y = line_width * outline_scale;
  thresholds.x = max(0.0, thresholds.y - LINE_FALLOFF);
  thresholds.w = line_width * 1.6;
  thresholds.z = max(0.0, thresholds.w - LINE_FALLOFF);

  /* Extend the primitive size by half line width on either side; odd for symmetry. */
  float ext_radius = floor(0.5 * v.size + 0.5) + thresholds.x;
  float point_size = ceil(ext_radius + thresholds.y) * 2.0 + 1.0;
  float4 radii = float4(ext_radius * DIAGONAL_SCALE,
                        ext_radius * CIRCLE_SCALE,
                        floor(ext_radius * SQUARE_SCALE + 0.5),
                        -LINE_FALLOFF);

  /* Quad corner: half the point size in pixels around the center. */
  pos.xy += v.corner * point_size / ViewportSize * pos.w;
  o.position = gpu_position(pos);
  o.color = v.color;
  o.outline_color = v.outline_color;
  o.radii = radii / point_size;
  o.thresholds = thresholds / point_size;
  o.coord = float2(v.corner.x * 0.5 + 0.5, 0.5 - v.corner.y * 0.5);
  o.shape = float4(flag(flags, SHAPE_DIAMOND),
                   flag(flags, SHAPE_CIRCLE),
                   flag(flags, SHAPE_CLIPPED_VERTICAL),
                   flag(flags, SHAPE_CLIPPED_HORIZONTAL));
  o.extra = float4(flag(flags, SHAPE_INNER_DOT),
                   flag(flags, SHAPE_ARROW_END_MAX),
                   flag(flags, SHAPE_ARROW_END_MIN),
                   flag(flags, SHAPE_ARROW_END_MIXED) > 0.5 ? 0.55 : 0.85);
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  float2 pos = v.coord - 0.5;
  float2 abs_pos = abs(pos);
  float4 radii = v.radii;
  float4 thresholds = v.thresholds;
  float center_dist = length(abs_pos);

  /* Distances to the outlines: diamond, circle, top & bottom clamp, left & right clamp. */
  float4 dists = float4((abs_pos.x + abs_pos.y) * DIAGONAL_SCALE - radii.x,
                        center_dist - radii.y,
                        abs_pos.y - radii.z,
                        abs_pos.x - radii.z);
  dists = v.shape > 0.5 ? dists : -1.0;
  float outline_dist = max(max(dists.x, dists.y), max(dists.z, dists.w));

  float outline_alpha = 1.0 - smoothstep(thresholds.x, thresholds.y, abs(outline_dist));

  /* Inside the outline: middle dot. */
  float dot_alpha = (1.0 - smoothstep(thresholds.z, thresholds.w, center_dist)) * v.extra.x;
  float alpha = max(outline_alpha, dot_alpha);
  /* Inside the outline: up and down arrow-like shading. */
  float ypos = max(v.extra.y > 0.5 ? pos.y : -1.0, v.extra.z > 0.5 ? -pos.y : -1.0);
  float minmax_dist = (ypos - radii.w) - abs_pos.x * MINMAX_BIAS;
  float minmax_step = smoothstep(thresholds.x, thresholds.y, minmax_dist * MINMAX_SCALE);
  alpha = max(alpha, minmax_step * v.extra.w * saturate(v.extra.y + v.extra.z));

  float4 inside = lerp(v.color, v.outline_color, alpha);
  float4 outside = float4(v.outline_color.rgb, v.outline_color.a * outline_alpha);
  return blender_srgb_to_framebuffer_space(outline_dist < 0.0 ? inside : outside);
}
