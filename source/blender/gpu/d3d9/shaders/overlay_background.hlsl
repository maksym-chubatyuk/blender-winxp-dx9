/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Viewport background (`overlay_background_frag.glsl`). The depth can't be sampled: "solid or
 * checker" always uses the solid color.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

/* Theme colors: background, background_gradient, checker_primary, checker_secondary. */
float4 uniform_buf__41[4];
/* Theme sizes: pixel ... checker. */
float4 uniform_buf__94[3];
float bg_type;
float4 color_override;
float vignette_enabled;
float vignette_aperture;
sampler2D color_buffer : register(s0);

struct VertIn {
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
  /* Fragment coordinates divided by the checker size. */
  float2 checker_co : TEXCOORD1;
  /* Solid color or linear gradient: affine in screen space, so exact per vertex. */
  float3 base_col : TEXCOORD2;
  /* Constant over the draw: is_radial, is_checker, is_mask, vignette_enabled. */
  float4 flags : TEXCOORD3;
};

/* The selection is done per vertex, the pixel shader is limited to 64 instructions which the R300
 * driver counts after its own translation. */
VertOut vert(VertIn v)
{
  VertOut o;
  float2 pos = drw_fullscreen_pos(v.vertex_id);
  float4 clip_pos = float4(pos, 1.0, 1.0);
  o.position = gpu_position(clip_pos);
  o.uv = pos * 0.5 + 0.5;
  float is_linear = (bg_type > 0.5 && bg_type < 1.5) ? 1.0 : 0.0;
  o.base_col = lerp(uniform_buf__41[0].rgb,
                    lerp(uniform_buf__41[1].rgb, uniform_buf__41[0].rgb, o.uv.y),
                    is_linear);
  o.flags = float4((bg_type > 2.5 && bg_type < 3.5) ? 1.0 : 0.0,
                   (bg_type > 1.5 && bg_type < 2.5) ? 1.0 : 0.0,
                   bg_type > 4.5 ? 1.0 : 0.0,
                   vignette_enabled > 0.5 ? 1.0 : 0.0);
  o.checker_co = gpu_frag_coord(clip_pos) / (uniform_buf__94[2].x * uniform_buf__94[0].x);
  return o;
}

#ifdef SIMPLE
/* `overlay_background_simple`: solid color or linear gradient (computed per vertex). */
float4 frag(VertOut v) : COLOR0
{
  float alpha = tex2D(color_buffer, v.uv).a;
  float3 bg_col = lerp(v.base_col, color_override.rgb, color_override.a);
  return float4(bg_col * (1.0 - alpha), (alpha == 1.0) ? 1.0 : 1.0 - alpha);
}
#else
float4 frag(VertOut v) : COLOR0
{
  float alpha = tex2D(color_buffer, v.uv).a;
  float dist = length(v.uv - 0.5);
  float vignette = smoothstep(vignette_aperture, vignette_aperture + 0.15, dist);
  alpha = lerp(alpha, vignette, v.flags.w);

  /* Gradients are interpolated in linear space (the gamma space version doesn't fit SM2.0). */
  float3 radial = lerp(uniform_buf__41[0].rgb, uniform_buf__41[1].rgb, dist * 1.41421356);
  float2 p = floor(v.checker_co);
  float3 checker = (frac((p.x + p.y) * 0.5) < 0.25) ? uniform_buf__41[2].rgb :
                                                       uniform_buf__41[3].rgb;
  float3 bg_col = lerp(v.base_col, radial, v.flags.x);
  bg_col = lerp(bg_col, checker, v.flags.y);
  bg_col = lerp(bg_col, color_override.rgb, color_override.a);

  /* Opaque: color * (1 - alpha), alpha forced to 1 where fully covered. */
  float4 color = float4(bg_col * (1.0 - alpha), (alpha == 1.0) ? 1.0 : 1.0 - alpha);
  color = (v.flags.w > 0.5) ? float4(bg_col, alpha) : color;
  return (v.flags.z > 0.5) ? float4((1.0 - alpha).xxx, 0.0) : color;
}
#endif
