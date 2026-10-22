/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared declarations of the Direct3D 9 shaders (vs_2_0 / ps_2_0).
 *
 * Conventions (see `d3d9_common.hh`):
 * - Uniforms and samplers use the names of the create info they replace. `int` and `bool` push
 *   constants are declared as `float`. Samplers use the register of their create info slot.
 * - Vertex attributes are the members of `struct VertIn`, with a `TEXCOORD<n>` semantic, `n` being
 *   the attribute location. `TEXCOORD15` is the vertex index (`gl_VertexID`).
 * - Vertex shaders pass their OpenGL clip space position through #gpu_position.
 */

/* Reserved constants, written by `D3D9Context::draw_setup`. */
float4 gpu_d3d9_pos_xform : register(c0); /* Y scale, Z scale, Z bias, instance index. */
float4 gpu_d3d9_pixel_ofs : register(c1); /* Half pixel offset in clip space. */

#define gpu_InstanceID gpu_d3d9_pos_xform.w

/* OpenGL clip space to D3D9: Y flip of off-screen targets, [-1..1] depth range, pixel centers. */
float4 gpu_position(float4 p)
{
  p.y *= gpu_d3d9_pos_xform.x;
  p.z = p.z * gpu_d3d9_pos_xform.y + p.w * gpu_d3d9_pos_xform.z;
  p.xy += gpu_d3d9_pixel_ofs.xy * p.w;
  return p;
}

/* sRGB render targets store linear values: there is no linear to sRGB encoding on write (and
 * they are sampled without decoding). Shaders writing sRGB colors (the UI) convert them when
 * `srgbTarget` is set, with a gamma 2 approximation to save instructions. */
float srgbTarget;
#define blender_srgb_to_framebuffer_space(col) \
  float4(lerp((col).rgb, (col).rgb * (col).rgb, srgbTarget), (col).a)

/* Pixel position in the viewport (OpenGL orientation) of a clip space position, for fragment
 * shaders using `gl_FragCoord` (ps_2_0 has no `VPOS`). Interpolate it as a varying. */
float2 gpu_frag_coord(float4 clip_pos)
{
  float2 viewport_size = 1.0 / abs(gpu_d3d9_pixel_ofs.xy);
  return (clip_pos.xy / clip_pos.w * 0.5 + 0.5) * viewport_size;
}

/* Size of one pixel in the units of the given axis of an orthographic projection. */
float gpu_pixel_size_y(float4x4 projection)
{
  return 2.0 * abs(gpu_d3d9_pixel_ofs.y) / max(abs(projection[1][1]), 1e-8);
}

/* Uniform buffers are copied to the float4 array constant named after the block, or from a given
 * register on to a constant named `<block>__<register>` (pixel shaders only have 32 constants).
 * When they hold integers, annotate the layout so they are converted to floats:
 * `d3d9-ubo <block name> <types>` comment line, one character per 32-bit component (f, i, u or b).
 *
 * Storage buffers can't be read by shaders. Data of the drawn instance is copied to constants:
 * `d3d9-resource-id <slot> <words per instance>` comment line: buffer of the draw manager resource ids
 *   (followed by the custom id when there are 2 words).
 * `d3d9-instance <constant> <slot> <element size> <offset> <index> <types>` comment line: copy the element
 *   (index `res`: resource id, `custom`: custom id, `instance`: instance index) of the storage
 *   buffer bound to the slot. Types are per 32-bit word (f, i, u, b), `B` unpacks a word into 4
 *   normalized bytes (4 components). */
