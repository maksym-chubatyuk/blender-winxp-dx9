/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Viewport grid and axes lines (`overlay_grid_vert.glsl`, `overlay_grid_frag.glsl`).
 * Integer bit logic is done with float math. Without line data output (no line anti-aliasing),
 * and without the stipple of faded lines.
 */

#include "d3d9_common.hlsl"
#define DRW_NO_MODEL
#include "draw_common.hlsl"

float4 view_buf[16];
/* OVERLAY_GridData: steps[8], (offset, clip_rect), (level, num_lines). */
// d3d9-ubo grid_buf ffff ffff ffff ffff ffff ffff ffff ffff ffff fuuu
float4 grid_buf[10];
/* size_viewport, size_viewport_inv; fresnel_mix_edit, pixel_fac. */
float4 uniform_buf__97[2];
/* Theme colors: grid, grid_emphasis, grid_axis_x/y/z. */
float4 uniform_buf__86[5];
float grid_flag;
float grid_iter;

#define STEPS_DRAW 3.0
#define STEPS_LEN 8.0

/* Bit `bit` (a power of two) of the flags. */
float flag(float flags, float bit)
{
  return fmod(floor(flags / bit), 2.0);
}

struct VertIn {
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float4 data : TEXCOORD1; /* coord, emphasis, alpha */
  /* The line is on the X or Y axis. Decided per vertex: the interpolated position isn't exact
   * with the 24 bit floats of R300 pixel shaders. */
  float2 on_axis : TEXCOORD2;
};

VertOut vert(VertIn v)
{
  VertOut o;
  o.position = float4(2.0, 2.0, 2.0, 1.0); /* Outside of the view: discard. */
  o.data = float4(0.0, 0.0, 0.0, 0.0);
  o.on_axis = float2(0.0, 0.0);

  float show_grid = flag(grid_flag, 1.0);
  float sima = flag(grid_flag, 512.0);
  float num_lines = grid_buf[9].y;
  float half_lines = max(floor(num_lines * 0.5), 1.0);

  float vid = v.vertex_id + 0.25;
  float side = fmod(floor(vid), 2.0);
  float pair = floor(vid * 0.5);
  float axis, line_level;
  float2 P;
  if (show_grid > 0.5) {
    axis = fmod(pair, 2.0);
    float index = floor(pair * 0.5);
    line_level = STEPS_DRAW - 1.0 - floor(index / num_lines);
    index = fmod(index, num_lines);
    P = float2(side > 0.5 ? half_lines : -half_lines, index - floor(num_lines * 0.5));
    P = axis > 0.5 ? P.yx : P;
  }
  else {
    axis = pair;
    line_level = STEPS_DRAW - 1.0;
    P = float2(half_lines * (side > 0.5 ? -1.0 : 1.0), 0.0);
  }

  /* Use the level offset in the 3D viewport. */
  float use_offset = (sima + flag(grid_flag, 256.0)) > 0.5 ? 0.0 : 1.0;
  float level = clamp(floor(grid_buf[9].x) + line_level - use_offset, 0.0, STEPS_LEN - 1.0);
  float step_axis = sima > 0.5 ? 1.0 - axis : axis;
  float4 steps = grid_buf[int(level)];
  float step_size = step_axis < 0.5 ? steps.x : (step_axis < 1.5 ? steps.y : steps.z);

  float2 step_offs = grid_buf[8].xy;
  if (show_grid > 0.5) {
    if (axis < 0.5) {
      step_offs.y = floor(step_offs.y / step_size + 0.5) * step_size;
    }
    else {
      step_offs.x = floor(step_offs.x / step_size + 0.5) * step_size;
    }
  }
  else {
    float3 view_pos = view_buf[DRW_VIEWINV + 3].xyz;
    step_offs = float2(axis < 0.5 ? view_pos.x : (axis < 1.5 ? view_pos.y : view_pos.z), 0.0);
  }

  bool is_persp = view_buf[DRW_WINMAT + 3].w == 0.0;
  float emphasis = saturate(line_level - frac(grid_buf[9].x));
  float alpha = saturate(line_level + 1.0 - frac(grid_buf[9].x));
  if (!is_persp) {
    alpha *= smoothstep(step_size * 0.25, step_size * 0.015625, uniform_buf__97[1].y);
  }
  float2 coord = P / half_lines;
  P = step_offs + step_size * P;

  float2 clip_rect = grid_buf[8].zw;
  float2 clip_min, clip_max;
  if (sima > 0.5) {
    clip_min = float2(-1.0, -1.0);
    clip_max = clip_rect * 2.0 - 1.0;
  }
  else if (show_grid > 0.5) {
    clip_min = step_offs - clip_rect;
    clip_max = step_offs + clip_rect;
  }
  else {
    float clip = clip_rect.x;
    if (axis > 0.5 && axis < 1.5) {
      /* Y axis: second component of the XY plane, first of YZ, none of XZ. */
      clip = flag(grid_flag, 128.0) > 0.5 ? clip_rect.x : clip_rect.y;
    }
    clip_min = float2(step_offs.x - clip, 0.0);
    clip_max = float2(step_offs.x + clip, 0.0);
  }
  if (all(P < clip_min) || all(P > clip_max)) {
    return o;
  }
  P = clamp(P, clip_min, clip_max);

  float3 pos = float3(0.0, 0.0, 0.0);
  if (show_grid > 0.5) {
    if (flag(grid_flag, 32.0) > 0.5) {
      pos.xy = P;
    }
    else if (flag(grid_flag, 64.0) > 0.5) {
      pos.xz = P;
    }
    else if (flag(grid_flag, 128.0) > 0.5) {
      pos.yz = P;
    }
    else {
      pos = float3(P * 0.5 + 0.5, flag(grid_flag, 2048.0) > 0.5 ? 0.45 : 0.76);
    }
  }
  else {
    float axis_flag = axis < 0.5 ? 4.0 : (axis < 1.5 ? 8.0 : 16.0);
    if (flag(grid_flag, axis_flag) < 0.5) {
      return o;
    }
    pos = float3(axis < 0.5 ? P.x : 0.0, (axis > 0.5 && axis < 1.5) ? P.x : 0.0, axis > 1.5 ? P.x : 0.0);
  }

  /* Lines of a higher level are drawn there. */
  if (show_grid > 0.5 && sima < 0.5 && line_level < STEPS_DRAW - 1.0 && level < STEPS_LEN - 1.0) {
    float4 next_steps = grid_buf[int(level) + 1];
    float next_step = axis < 0.5 ? next_steps.x : next_steps.y;
    float j = abs(axis < 0.5 ? P.y : P.x) / next_step;
    if (abs(floor(j) - j) <= 1e-4) {
      return o;
    }
  }

  float4 clip_pos = drw_mat_mul(view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(pos, 1.0)));
  if (is_persp || sima > 0.5) {
    float z_factor = (grid_iter * STEPS_DRAW + line_level) / (4.0 * STEPS_DRAW);
    clip_pos.z += lerp(0.00025, -0.00020, z_factor);
  }
  else if (flag(grid_flag, 4096.0) > 0.5) {
    clip_pos.z = 1.0;
  }
  o.position = gpu_position(clip_pos);
  o.data = float4(coord, emphasis, alpha);
  /* Relative to the line spacing: the line positions don't cancel out exactly with the fused
   * multiply-adds of the hardware. */
  float axis_epsilon = step_size * 1e-3;
  o.on_axis = float2(
      (flag(grid_flag, 4.0) > 0.5 && all(abs(pos.yz) <= axis_epsilon)) ? 1.0 : 0.0,
      (flag(grid_flag, 8.0) > 0.5 && all(abs(pos.xz) <= axis_epsilon)) ? 1.0 : 0.0);
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

FragOut frag(VertOut v)
{
  FragOut o;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
  float4 color = lerp(uniform_buf__86[0], uniform_buf__86[1], v.data.z);
  float on_axis_x = v.on_axis.x;
  float on_axis_y = v.on_axis.y;
  if (on_axis_x + on_axis_y > 0.5 && flag(grid_flag, 1.0) > 0.5) {
    clip(-1.0);
  }
  color = on_axis_x > 0.5 ? uniform_buf__86[2] : (on_axis_y > 0.5 ? uniform_buf__86[3] : color);
  color.a *= v.data.w;
  /* Fade at the edge of the grid level. */
  color.a *= 1.0 - min(1.0, length(v.data.xy));
  /* Lower iterations of the perspective grid have a stronger alpha. */
  color.a *= exp2(-grid_iter);
  clip(color.a - 0.002);
  o.color = color;
  return o;
}
