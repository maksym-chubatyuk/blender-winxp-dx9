/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Mesh edit mode overlays (`overlay_edit_mesh_*`). Defines: VERT, EDGE, FACE, FACEDOT, DEPTH.
 *
 * D3D9 differences:
 * - Edges are 1 pixel lines drawn from the edge batch (no expansion to quads): the seam / sharp /
 *   crease colors replace the wire color of unselected edges instead of being drawn around them.
 * - The depth can't be sampled: no dimming of occluded vertices and edges in X-ray.
 * - No retopology offset.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"

// d3d9-resource-id 11 1
float4 view_buf[12];
/* Theme colors, `ThemeColors` from `wire_edit` (1) to `edit_mesh_middle` (46). */
float4 uniform_buf__1[46];
/* Theme sizes: light_circle_shadow, vert, edge, face_dot. */
float4 uniform_buf__95;
/* fresnel_mix_edit, pixel_fac. */
float4 uniform_buf__98;

float ndc_offset_factor;
float ndc_offset;
float wire_shading;
float select_face;
float select_edge;
float alpha;
float4 data_mask;
float use_vertex_selection;

#define THEME(index) uniform_buf__1[(index) - 1]
#define TH_WIRE_EDIT THEME(1)
#define TH_VERT THEME(12)
#define TH_VERT_SELECT THEME(13)
#define TH_EDIT_MESH_ACTIVE THEME(16)
#define TH_EDGE_SELECT THEME(17)
#define TH_EDGE_MODE_SELECT THEME(18)
#define TH_EDGE_SEAM THEME(19)
#define TH_EDGE_SHARP THEME(20)
#define TH_EDGE_CREASE THEME(21)
#define TH_EDGE_BWEIGHT THEME(22)
#define TH_EDGE_FREESTYLE THEME(24)
#define TH_FACE THEME(25)
#define TH_FACE_SELECT THEME(26)
#define TH_FACE_MODE_SELECT THEME(27)
#define TH_FACE_FREESTYLE THEME(29)
#define TH_FACEDOT THEME(36)
#define TH_EDIT_MESH_MIDDLE THEME(46)

/* Bit `bit` (a power of two) of the flags, as 0 or 1. */
float flag(float flags, float bit)
{
  return fmod(floor(flags / bit), 2.0);
}

/* Bit of the edit data, after the overlay mask. */
float masked_flag(float flags, float mask, float bit)
{
  return flag(flags, bit) * flag(mask, bit);
}

float3 non_linear_blend_color(float3 col1, float3 col2, float fac)
{
  col1 = pow(abs(col1), 1.0 / 2.2);
  col2 = pow(abs(col2), 1.0 / 2.2);
  return pow(abs(lerp(col1, col2, fac)), 2.2);
}

struct VertIn {
  float3 pos : TEXCOORD0;
  float4 data : TEXCOORD1;
#if defined(FACEDOT)
  float4 nor_and_flag : TEXCOORD2;
#elif !defined(FACE) && !defined(DEPTH)
  float3 vnor : TEXCOORD2;
#endif
};

#if defined(VERT) || defined(FACEDOT)
// d3d9-point-sprite
struct VertOut {
  float4 position : POSITION;
  float point_size : PSIZE;
  float4 color : COLOR0;
  /* Crease color and amount (vertices). */
  float4 crease : COLOR1;
};
#else
struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
};
#endif

VertOut vert(VertIn v)
{
  VertOut o;
  float3 world_pos = drw_point_object_to_world(v.pos);
  float4 view_pos = drw_mat_mul(view_buf, DRW_VIEWMAT, float4(world_pos, 1.0));
  float4 clip_pos = drw_mat_mul(view_buf, DRW_WINMAT, view_pos);
  float is_persp = view_buf[DRW_WINMAT + 3].w == 0.0 ? 1.0 : 0.0;

  float4 color = float4(0.0, 0.0, 0.0, 0.0);
#if defined(VERT)
  float crease = masked_flag(v.data.z, data_mask.z, 16.0) * 1.0 +
                 masked_flag(v.data.z, data_mask.z, 32.0) * 2.0 +
                 masked_flag(v.data.z, data_mask.z, 64.0) * 4.0 +
                 masked_flag(v.data.z, data_mask.z, 128.0) * 8.0;
  crease /= 15.0;
  float active = masked_flag(v.data.y, data_mask.y, 1.0);
  float selected = masked_flag(v.data.y, data_mask.y, 2.0);
  color = lerp(TH_VERT, TH_EDGE_CREASE, crease);
  color = selected > 0.5 ? TH_VERT_SELECT : color;
  color = active > 0.5 ? float4(TH_EDIT_MESH_ACTIVE.rgb, 1.0) : color;
  o.point_size = uniform_buf__95.y * (crease > 0.0 ? 3.0 : 2.0);
  o.crease = float4(TH_EDGE_CREASE.rgb, crease);
  /* Make selected and active vertex always on top. */
  clip_pos.z -= 5e-7 * abs(clip_pos.w) * (flag(v.data.x, 2.0) + flag(v.data.x, 1.0));
#elif defined(EDGE)
  float edge_selected = masked_flag(v.data.y, data_mask.y, 16.0);
  float4 selected_col = select_edge > 0.5 ? TH_EDGE_MODE_SELECT : TH_EDGE_SELECT;
  float4 inner;
  float is_selected;
  if (use_vertex_selection > 0.5) {
    is_selected = saturate(masked_flag(v.data.y, data_mask.y, 1.0) +
                           masked_flag(v.data.y, data_mask.y, 2.0));
    inner = is_selected > 0.5 ? selected_col : TH_WIRE_EDIT;
  }
  else {
    is_selected = edge_selected;
    inner = is_selected > 0.5 ? selected_col : TH_WIRE_EDIT;
    inner = masked_flag(v.data.y, data_mask.y, 8.0) > 0.5 ? TH_EDIT_MESH_ACTIVE : inner;
  }
  /* `EDIT_MESH_edge_color_outer`: shown instead of the wire color. */
  float crease = fmod(v.data.z, 16.0) * flag(data_mask.z, 1.0) / 15.0;
  float bweight = v.data.w * flag(data_mask.w, 1.0) / 255.0;
  float4 outer = float4(0.0, 0.0, 0.0, 0.0);
  outer = masked_flag(v.data.y, data_mask.y, 128.0) > 0.5 ? TH_EDGE_FREESTYLE : outer;
  outer = masked_flag(v.data.y, data_mask.y, 64.0) > 0.5 ? TH_EDGE_SHARP : outer;
  outer = crease > 0.0 ? float4(TH_EDGE_CREASE.rgb, 1.0) : outer;
  outer = bweight > 0.0 ? float4(TH_EDGE_BWEIGHT.rgb, 1.0) : outer;
  outer = masked_flag(v.data.y, data_mask.y, 32.0) > 0.5 ? TH_EDGE_SEAM : outer;
  color = (outer.a > 0.0 && is_selected < 0.5) ? outer : inner;
  color.a = 1.0;
  clip_pos.z -= outer.a > 0.0 ? 5e-7 * abs(clip_pos.w) : 0.0;
#elif defined(FACE)
  float face_active = masked_flag(v.data.x, data_mask.x, 1.0);
  float face_selected = masked_flag(v.data.x, data_mask.x, 2.0);
  float face_freestyle = masked_flag(v.data.x, data_mask.x, 4.0);
  float4 selected_face_col = select_face > 0.5 ? TH_FACE_MODE_SELECT : TH_FACE_SELECT;
  color = face_freestyle > 0.5 ? TH_FACE_FREESTYLE : TH_FACE;
  color = face_selected > 0.5 ? selected_face_col : color;
  if (select_face > 0.5 && face_active > 0.5) {
    color = float4(lerp(selected_face_col.rgb, TH_EDIT_MESH_ACTIVE.rgb, 0.5), selected_face_col.a);
  }
  if (wire_shading > 0.5) {
    /* Lower face selection opacity for better wireframe visibility. */
    color.a *= face_selected > 0.5 ? 0.6 : 1.0;
  }
  else {
    float shown = select_face + face_selected + face_active + face_freestyle;
    color.a = shown > 0.5 ? color.a : 0.0;
  }
  /* Faces count as occluded. */
  color.a *= alpha;
#elif defined(FACEDOT)
  float facedot_flag = v.nor_and_flag.w;
  color = facedot_flag > 0.0 ? TH_FACEDOT : TH_VERT;
  color = facedot_flag < 0.0 ? float4(TH_EDIT_MESH_ACTIVE.rgb, 1.0) : color;
  o.point_size = uniform_buf__95.w;
  o.crease = float4(0.0, 0.0, 0.0, 0.0);
  /* Bias Face-dot Z position in clip-space. */
  clip_pos.z -= is_persp > 0.5 ? 0.00035 : 1e-6;
#endif

#if !defined(FACE) && !defined(DEPTH)
  /* Facing based color blend. */
#  if defined(FACEDOT)
  float3 lN = v.nor_and_flag.xyz;
#  else
  float3 lN = v.vnor;
#  endif
  float3 view_normal = normalize(drw_mat_mul3(view_buf, DRW_VIEWMAT, drw_normal_object_to_world(lN)) +
                                 1e-4);
  float3 view_vec = is_persp > 0.5 ? normalize(view_pos.xyz) : float3(0.0, 0.0, 1.0);
  float facing = 1.0 - abs(dot(view_vec, view_normal)) * 0.2;
  color.rgb = lerp(color.rgb,
                   non_linear_blend_color(TH_EDIT_MESH_MIDDLE.rgb, color.rgb, facing),
                   uniform_buf__98.x);
#endif

  clip_pos.z -= ndc_offset_factor * ndc_offset;
  o.position = gpu_position(clip_pos);
  o.color = saturate(color);
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

#if defined(VERT) || defined(FACEDOT)
FragOut frag(VertOut v, float2 point_coord : TEXCOORD0)
{
  FragOut o;
  float2 centered = point_coord - 0.5;
  float dist_squared = dot(centered, centered);
  /* Round point with jagged edges. */
  clip(0.25 - dist_squared);
  o.color = v.color;
  /* Crease ring of vertices. */
  o.color.rgb = (v.crease.a > 0.0 && dist_squared > 0.125) ?
                    lerp(v.color.rgb, v.crease.rgb, v.crease.a) :
                    o.color.rgb;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
  return o;
}
#else
FragOut frag(VertOut v)
{
  FragOut o;
  o.color = v.color;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
  return o;
}
#endif
