/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * MECHAZYIU shaders. They only exist in HLSL for the Direct3D 9 backend
 * (`gpu/d3d9/shaders/mechazyiu_*.hlsl`), these create infos describe their resources.
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "draw_object_infos_infos.hh"
#  include "draw_view_infos.hh"
#endif

#include "gpu_shader_create_info.hh"

/* G-buffer of the surfaces: 4 render targets. */
GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_common)
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float3, nor)
VERTEX_SOURCE("mechazyiu_d3d9_vert.glsl")
FRAGMENT_SOURCE("mechazyiu_d3d9_frag.glsl")
/* rgb base color (multiplies the texture), a alpha. */
PUSH_CONSTANT(float4, mat_base)
/* metallic, roughness, dielectric F0, material index / 255. */
PUSH_CONSTANT(float4, mat_params)
FRAGMENT_OUT(0, float4, out_diffuse)
FRAGMENT_OUT(1, float2, out_normal)
FRAGMENT_OUT(2, float, out_depth)
FRAGMENT_OUT(3, float4, out_specular)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_texture_common)
VERTEX_IN(2, float2, au)
SAMPLER(0, sampler2D, base_tx)
ADDITIONAL_INFO(mechazyiu_gbuffer_common)
GPU_SHADER_CREATE_END()

/* The `_flip` variants draw the back faces (culled front faces), with flipped normals: ps_2_0
 * can't tell back faces apart. */
GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer)
ADDITIONAL_INFO(mechazyiu_gbuffer_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_flip)
ADDITIONAL_INFO(mechazyiu_gbuffer_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_texture)
ADDITIONAL_INFO(mechazyiu_gbuffer_texture_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_texture_flip)
ADDITIONAL_INFO(mechazyiu_gbuffer_texture_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* The `_single` variants write one target per draw (hardware without multiple render targets). */
GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_single_common)
/* One-hot selection of the target written. */
PUSH_CONSTANT(float4, gbuffer_target)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_single)
ADDITIONAL_INFO(mechazyiu_gbuffer_common)
ADDITIONAL_INFO(mechazyiu_gbuffer_single_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_flip_single)
ADDITIONAL_INFO(mechazyiu_gbuffer_common)
ADDITIONAL_INFO(mechazyiu_gbuffer_single_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_texture_single)
ADDITIONAL_INFO(mechazyiu_gbuffer_texture_common)
ADDITIONAL_INFO(mechazyiu_gbuffer_single_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_texture_flip_single)
ADDITIONAL_INFO(mechazyiu_gbuffer_texture_common)
ADDITIONAL_INFO(mechazyiu_gbuffer_single_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Dithered transparency: depth of the covered pixels (alpha tested against noise). */
GPU_SHADER_CREATE_INFO(mechazyiu_alpha_common)
/* xy: half the target size in pixels, z: pixel offset of the noise, w: threshold offset (the
 * sample). The texture's alpha is the coverage when `mat_params.w` is 1. */
PUSH_CONSTANT(float4, alpha_params)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_alpha_prepass)
ADDITIONAL_INFO(mechazyiu_gbuffer_common)
ADDITIONAL_INFO(mechazyiu_alpha_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_alpha_prepass_texture)
ADDITIONAL_INFO(mechazyiu_gbuffer_texture_common)
ADDITIONAL_INFO(mechazyiu_alpha_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Blended transparency: surfaces over the lit sample, then each light added. */
GPU_SHADER_CREATE_INFO(mechazyiu_forward_common)
VERTEX_IN(0, float3, pos)
VERTEX_IN(1, float3, nor)
VERTEX_SOURCE("mechazyiu_d3d9_vert.glsl")
FRAGMENT_SOURCE("mechazyiu_d3d9_frag.glsl")
/* rgb: base color, a: alpha. */
PUSH_CONSTANT(float4, mat_base)
/* metallic, Blinn-Phong exponent, dielectric F0, 1 when the texture's alpha is the coverage. */
PUSH_CONSTANT(float4, mat_params)
/* x: 1 for perspective projections, zw: part of the tile buffers used. */
PUSH_CONSTANT(float4, proj_type)
/* The sample under the surface (when not blended by the hardware). */
SAMPLER(1, sampler2D, sample_tx)
FRAGMENT_OUT(0, float4, out_color)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_base)
PUSH_CONSTANT_ARRAY(float4, world_sh, 4)
/* rgb: emission. */
PUSH_CONSTANT(float4, emission)
ADDITIONAL_INFO(mechazyiu_forward_common)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_light_common)
PUSH_CONSTANT(float4, light_position)
PUSH_CONSTANT(float4, light_direction)
PUSH_CONSTANT(float4, light_color)
PUSH_CONSTANT(float4, light_spot)
ADDITIONAL_INFO(mechazyiu_forward_common)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_texture_common)
VERTEX_IN(2, float2, au)
SAMPLER(0, sampler2D, base_tx)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward)
ADDITIONAL_INFO(mechazyiu_forward_base)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_blend)
ADDITIONAL_INFO(mechazyiu_forward_base)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_texture)
ADDITIONAL_INFO(mechazyiu_forward_base)
ADDITIONAL_INFO(mechazyiu_forward_texture_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_texture_blend)
ADDITIONAL_INFO(mechazyiu_forward_base)
ADDITIONAL_INFO(mechazyiu_forward_texture_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_light)
ADDITIONAL_INFO(mechazyiu_forward_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_light_blend)
ADDITIONAL_INFO(mechazyiu_forward_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_light_texture)
ADDITIONAL_INFO(mechazyiu_forward_light_common)
ADDITIONAL_INFO(mechazyiu_forward_texture_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_forward_light_texture_blend)
ADDITIONAL_INFO(mechazyiu_forward_light_common)
ADDITIONAL_INFO(mechazyiu_forward_texture_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Passes over a rectangle of the tile, reading the G-buffer. */
GPU_SHADER_CREATE_INFO(mechazyiu_screen_common)
VERTEX_SOURCE("mechazyiu_d3d9_vert.glsl")
FRAGMENT_SOURCE("mechazyiu_d3d9_frag.glsl")
/* Rectangle drawn, in normalized device coordinates (xmin, ymin, xmax, ymax). */
PUSH_CONSTANT(float4, rect)
/* Projection of the tile: (1 / P00, 1 / P11, P20 or -P30, P21 or -P31), see `mechazyiu_common.hlsl`. */
PUSH_CONSTANT(float4, proj)
/* x: 1 for perspective projections, zw: part of the textures used. */
PUSH_CONSTANT(float4, proj_type)
FRAGMENT_OUT(0, float4, out_color)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_gbuffer_read)
SAMPLER(0, sampler2D, gb_diffuse_tx)
SAMPLER(1, sampler2D, gb_normal_tx)
SAMPLER(2, sampler2D, gb_depth_tx)
SAMPLER(3, sampler2D, gb_specular_tx)
SAMPLER(4, sampler2D, accum_tx)
ADDITIONAL_INFO(mechazyiu_screen_common)
GPU_SHADER_CREATE_END()

/* First pass of a sample: world light, emission and the background. */
GPU_SHADER_CREATE_INFO(mechazyiu_base)
SAMPLER(5, sampler2D, material_tx)
/* World lighting, first order spherical harmonics of the irradiance divided by pi:
 * E(n) / pi = world_sh[0] + n.x * world_sh[1] + n.y * world_sh[2] + n.z * world_sh[3] (in view
 * space). world_sh[0].a: 1 when the background is transparent. */
PUSH_CONSTANT_ARRAY(float4, world_sh, 4)
/* Background color (rgb). */
PUSH_CONSTANT(float4, background)
ADDITIONAL_INFO(mechazyiu_gbuffer_read)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* One light: adds its contribution over the rectangle it can reach. */
GPU_SHADER_CREATE_INFO(mechazyiu_light_common)
/* View space position (point, spot, area) or direction toward the light (sun);
 * w: 1 / influence radius squared. */
PUSH_CONSTANT(float4, light_position)
/* View space direction the light points to (spot, area), w: unused. */
PUSH_CONSTANT(float4, light_direction)
/* Radiant intensity (or irradiance for suns) / pi, w: specular factor / 4
 * (the 4 of the specular BRDF denominator). */
PUSH_CONSTANT(float4, light_color)
/* Spot: x = 1 / (cos blend start - cos size), y = -cos size * x. z: diffuse factor. */
PUSH_CONSTANT(float4, light_spot)
/* Shadow of the light, or a white texture. */
SAMPLER(5, sampler2D, shadow_mask_tx)
ADDITIONAL_INFO(mechazyiu_gbuffer_read)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_light_point)
ADDITIONAL_INFO(mechazyiu_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Added by blending (floating point blending hardware). */
GPU_SHADER_CREATE_INFO(mechazyiu_light_point_blend)
ADDITIONAL_INFO(mechazyiu_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_light_spot)
ADDITIONAL_INFO(mechazyiu_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Added by blending (floating point blending hardware). */
GPU_SHADER_CREATE_INFO(mechazyiu_light_spot_blend)
ADDITIONAL_INFO(mechazyiu_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_light_sun)
ADDITIONAL_INFO(mechazyiu_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Added by blending (floating point blending hardware). */
GPU_SHADER_CREATE_INFO(mechazyiu_light_sun_blend)
ADDITIONAL_INFO(mechazyiu_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_light_area)
ADDITIONAL_INFO(mechazyiu_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Added by blending (floating point blending hardware). */
GPU_SHADER_CREATE_INFO(mechazyiu_light_area_blend)
ADDITIONAL_INFO(mechazyiu_light_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Shadow maps: the surfaces seen from a light, normalized distance to it (orthographic: depth). */
GPU_SHADER_CREATE_INFO(mechazyiu_shadow_common)
VERTEX_IN(0, float3, pos)
VERTEX_SOURCE("mechazyiu_d3d9_vert.glsl")
FRAGMENT_SOURCE("mechazyiu_d3d9_frag.glsl")
/* xyz: light position (world space), w: 1 / far distance. */
PUSH_CONSTANT(float4, shadow_light)
FRAGMENT_OUT(0, float, out_distance)
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_modelmat)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow)
ADDITIONAL_INFO(mechazyiu_shadow_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_ortho)
ADDITIONAL_INFO(mechazyiu_shadow_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Dithered transparent casters. */
GPU_SHADER_CREATE_INFO(mechazyiu_shadow_alpha_common)
PUSH_CONSTANT(float4, mat_base)
PUSH_CONSTANT(float4, mat_params)
ADDITIONAL_INFO(mechazyiu_alpha_common)
ADDITIONAL_INFO(mechazyiu_shadow_common)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_alpha_texture_common)
VERTEX_IN(2, float2, au)
SAMPLER(0, sampler2D, base_tx)
ADDITIONAL_INFO(mechazyiu_shadow_alpha_common)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_alpha)
ADDITIONAL_INFO(mechazyiu_shadow_alpha_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_alpha_texture)
ADDITIONAL_INFO(mechazyiu_shadow_alpha_texture_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_ortho_alpha)
ADDITIONAL_INFO(mechazyiu_shadow_alpha_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_ortho_alpha_texture)
ADDITIONAL_INFO(mechazyiu_shadow_alpha_texture_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Shadow of a light over its rectangle, for the light pass. */
GPU_SHADER_CREATE_INFO(mechazyiu_shadow_mask_common)
SAMPLER(1, sampler2D, gb_normal_tx)
SAMPLER(2, sampler2D, gb_depth_tx)
SAMPLER(6, sampler2D, shadow_tx)
/* View space light position, w: 1 / far distance. */
PUSH_CONSTANT(float4, light_position)
/* Spot and sun: view space to the light's clip space (columns). */
PUSH_CONSTANT_ARRAY(float4, shadow_mat, 4)
/* xy: lookup offset, z: normal offset per unit of distance, w: constant normal offset. */
PUSH_CONSTANT(float4, shadow_params)
/* Depth bias: lit when `ref * x - y <= occluder`. */
PUSH_CONSTANT(float4, shadow_bias)
ADDITIONAL_INFO(mechazyiu_screen_common)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_mask_cube)
ADDITIONAL_INFO(mechazyiu_shadow_mask_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_mask_spot)
ADDITIONAL_INFO(mechazyiu_shadow_mask_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(mechazyiu_shadow_mask_sun)
ADDITIONAL_INFO(mechazyiu_shadow_mask_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Copy of a rectangle of a texture: `uv_transform` maps the rectangle's uv to the source. */
GPU_SHADER_CREATE_INFO(mechazyiu_copy)
SAMPLER(0, sampler2D, source_tx)
PUSH_CONSTANT(float4, uv_transform)
ADDITIONAL_INFO(mechazyiu_screen_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* A sample mixed into the running mean (read through `uv_transform`). */
GPU_SHADER_CREATE_INFO(mechazyiu_mix)
SAMPLER(0, sampler2D, source_tx)
SAMPLER(1, sampler2D, mean_tx)
PUSH_CONSTANT(float4, uv_transform)
/* x: weight of the sample. */
PUSH_CONSTANT(float4, mix_weight)
ADDITIONAL_INFO(mechazyiu_screen_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* Viewport output: color of the mean and depth of the last sample's G-buffer. */
GPU_SHADER_CREATE_INFO(mechazyiu_output)
SAMPLER(0, sampler2D, source_tx)
SAMPLER(1, sampler2D, depth_tx)
PUSH_CONSTANT(float4, uv_transform)
/* Viewport projection (not jittered): P22, P32, 1 for perspective. */
PUSH_CONSTANT(float4, depth_proj)
DEPTH_WRITE(DepthWrite::ANY)
ADDITIONAL_INFO(mechazyiu_screen_common)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
