/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Images in the image editor (`image_engine_image_shader`): a full-screen triangle sampling the
 * image through the screen to image matrix (render results, textures). The tiled (UDIM) and the
 * screen space variants (images bigger than a texture) aren't supported.
 */

#include "d3d9_common.hlsl"
#define DRW_NO_MODEL
#include "draw_common.hlsl"

float4x4 image_matrix;
float4 shuffle;
float2 far_near_distances;
float draw_flags;
float is_image_premultiplied;
float is_repeated;
sampler2D image_tx : register(s0);

#define IMAGE_DRAW_FLAG_SHOW_ALPHA 1.0
#define IMAGE_DRAW_FLAG_APPLY_ALPHA 2.0
#define IMAGE_DRAW_FLAG_SHUFFLING 4.0
#define IMAGE_DRAW_FLAG_DEPTH 8.0
#define Z_DEPTH_BORDER 1.0
#define Z_DEPTH_IMAGE 0.75

float flag(float flags, float bit)
{
  return fmod(floor(flags / bit), 2.0);
}

struct VertIn {
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  /* Image coordinates of the pixel. */
  float2 coordinates : TEXCOORD0;
};

VertOut vert(VertIn v)
{
  VertOut o;
  float2 pos = drw_fullscreen_pos(v.vertex_id);
  o.position = gpu_position(float4(pos, 0.0, 1.0));
  const float2 screen_uv = pos * 0.5 + 0.5;
  o.coordinates = mul((float3x3)image_matrix, float3(screen_uv, 1.0)).xy;
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float depth : DEPTH;
};

FragOut frag(VertOut v)
{
  FragOut o;
  const float2 coordinates = v.coordinates;
  const bool outside = is_repeated < 0.5 &&
                       (any(coordinates < 0.0) || any(coordinates > 1.0));
  float4 color = tex2D(image_tx, coordinates);
  if (flag(draw_flags, IMAGE_DRAW_FLAG_APPLY_ALPHA) > 0.5 && is_image_premultiplied < 0.5) {
    color.rgb *= color.a;
  }
  if (flag(draw_flags, IMAGE_DRAW_FLAG_DEPTH) > 0.5) {
    color = smoothstep(far_near_distances.x, far_near_distances.y, color);
  }
  if (flag(draw_flags, IMAGE_DRAW_FLAG_SHUFFLING) > 0.5) {
    color = dot(color, shuffle);
  }
  if (flag(draw_flags, IMAGE_DRAW_FLAG_SHOW_ALPHA) < 0.5) {
    color.a = 1.0;
  }
  o.color = outside ? float4(0.0, 0.0, 0.0, 0.0) : color;
  o.depth = outside ? Z_DEPTH_BORDER : Z_DEPTH_IMAGE;
  return o;
}
