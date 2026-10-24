/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared parts of the MECHAZYIU render engine shaders (`draw/engines/mechazyiu`).
 *
 * G-buffer (all 32 bit per pixel, a requirement of multiple render targets on R300):
 * - 0 (RGBA8): sqrt(diffuse color), roughness (at least 0.045).
 * - 1 (RGB10_A2): view space normal * 0.5 + 0.5.
 * - 2 (R32F): linear depth (distance along the view axis), 0 for the background.
 * - 3 (RGBA8): sqrt(specular color at normal incidence), material index / 255.
 *
 * Passes over the G-buffer draw a rectangle (`rect`, in normalized device coordinates) and get
 * the view space position of their pixels from the linear depth: the view ray of a pixel is
 * linear in screen space for both projection types (`proj`, `proj_type`).
 */

/* 10 bits per component: precise enough without renormalizing (one instruction to decode, the
 * light passes are at the ps_2_0 limit). */
float3 mechazyiu_normal_decode(float3 e)
{
  return e * 2.0 - 1.0;
}

/**
 * Dithered transparency: the value to alpha test (kept from 0.5) for a coverage `alpha`, against
 * the interleaved gradient noise of the pixel (from its clip position), moved each sample:
 * `alpha_params` xy: half the target size in pixels, z: pixel offset of the pattern, w: offset of
 * the threshold.
 */
float mechazyiu_coverage(float alpha, float3 clip_xyw, float4 alpha_params)
{
  float2 pixel = clip_xyw.xy / clip_xyw.z * alpha_params.xy + alpha_params.zz * float2(1.0, 0.618);
  float noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))) +
                     alpha_params.w);
  return alpha - noise + 0.5;
}

/**
 * Shadow map distances (in [0..1]) in the 24 bits of an 8 bit color: the alpha stays free for the
 * alpha test of the dithered casters (it has no effect on single channel float targets).
 */
float3 mechazyiu_distance_encode(float d)
{
  float3 e = frac(d * float3(1.0, 255.0, 65025.0));
  e.xy -= e.yz * (1.0 / 255.0);
  return e;
}

float mechazyiu_distance_decode(float3 e)
{
  return dot(e, float3(1.0, 1.0 / 255.0, 1.0 / 65025.0));
}

#ifdef MECHAZYIU_SCREEN_PASS
float4 rect;
float4 proj;
/* x: 1 for perspective, zw: part of the textures used (tiles smaller than their buffers). */
float4 proj_type;

struct VertIn {
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
  float3 ray_origin : TEXCOORD1;
  float3 ray_dir : TEXCOORD2;
};

VertOut vert(VertIn v)
{
  /* Two triangles: corners (0,0) (1,0) (0,1), (1,0) (1,1) (0,1). */
  float id = v.vertex_id;
  float2 corner;
  corner.x = saturate(1.0 - abs(id - 1.0)) + saturate(1.0 - abs(id - 3.0)) +
             saturate(1.0 - abs(id - 4.0));
  corner.y = saturate(1.0 - abs(id - 2.0)) + saturate(1.0 - abs(id - 4.0)) +
             saturate(1.0 - abs(id - 5.0));
  float2 ndc = lerp(rect.xy, rect.zw, corner);

  VertOut o;
  o.position = gpu_position(float4(ndc, 0.0, 1.0));
  o.uv = (ndc * 0.5 + 0.5) * proj_type.zw;
  /* Perspective: x = depth * (ndc.x + P20) / P00. Orthographic: x = (ndc.x - P30) / P00. */
  float2 xy = (ndc + proj.zw) * proj.xy;
  float persp = proj_type.x;
  o.ray_origin = float3(xy * (1.0 - persp), 0.0);
  o.ray_dir = float3(xy * persp, -1.0);
  return o;
}
#endif
