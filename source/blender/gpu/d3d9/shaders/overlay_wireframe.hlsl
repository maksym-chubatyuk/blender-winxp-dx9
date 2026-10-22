/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Object wireframes (`overlay_wireframe`): wireframe shading, the wireframe overlay and the
 * wire display type.
 *
 * D3D9 differences: 1 pixel lines drawn from the edge batch, no custom depth bias (needs the
 * depth), no hair.
 */

#include "d3d9_common.hlsl"
#include "draw_common.hlsl"
#include "select_common.hlsl"

#ifdef SELECT
// d3d9-resource-id 11 2
// d3d9-instance select_custom_id 11 8 4 instance u
float4 select_custom_id;
#else
// d3d9-resource-id 11 1
#endif
float4 view_buf[12];
/* ObjectInfos of the instance: ob_color; index, light set, random, flag. */
// d3d9-instance ob_info 9 80 32 res ffffuufu
float4 ob_info[2];
/* Theme colors: wire (0) to background (41). */
float4 uniform_buf__0[42];
/* size_viewport, size_viewport_inv. */
float4 uniform_buf__97;

float ndc_offset_factor;
float wire_step_param;
float wire_opacity;
float use_coloring;
float is_transform;
float color_type;

#define TH_WIRE uniform_buf__0[0]
#define TH_ACTIVE_OBJECT uniform_buf__0[2]
#define TH_OBJECT_SELECT uniform_buf__0[3]
#define TH_TRANSFORM uniform_buf__0[6]
#define TH_BACKGROUND uniform_buf__0[41]

#define V3D_SHADING_RANDOM_COLOR 1.0
#define V3D_SHADING_SINGLE_COLOR 2.0
#define V3D_SHADING_OBJECT_COLOR 4.0

float flag(float flags, float bit)
{
  return fmod(floor(flags / bit), 2.0);
}

float3 hsv_to_rgb(float3 hsv)
{
  float3 nrgb = abs(hsv.x * 6.0 - float3(3.0, 2.0, 4.0)) * float3(1.0, -1.0, -1.0) +
                float3(-1.0, 2.0, 2.0);
  nrgb = saturate(nrgb);
  return ((nrgb - 1.0) * hsv.y + 1.0) * hsv.z;
}

struct VertIn {
  float3 pos : TEXCOORD0;
  float3 nor : TEXCOORD1;
  float wd : TEXCOORD2;
};

struct VertOut {
  float4 position : POSITION;
  /* Alpha: 1 when the edge is visible (wireframe threshold). */
  float4 color : TEXCOORD0;
  float visible : TEXCOORD1;
  SELECT_VARYING
};

VertOut vert(VertIn v)
{
  VertOut o;
  float ob_flag = ob_info[1].w;
  float is_selected = flag(ob_flag, 1.0);
  float is_from_set = flag(ob_flag, 4.0);
  float is_active = flag(ob_flag, 8.0);
  float no_nor_facing = (color_type == V3D_SHADING_SINGLE_COLOR) ? 0.0 : 0.5;

  float3 wpos = drw_point_object_to_world(v.pos);
  float3 wnor = drw_normal_object_to_world(v.nor);
  wnor *= rsqrt(max(dot(wnor, wnor), 1e-20));
  bool is_persp = view_buf[DRW_WINMAT + 3].w == 0.0;
  float3 V = is_persp ? normalize(view_buf[DRW_VIEWINV + 3].xyz - wpos) :
                        view_buf[DRW_VIEWINV + 2].xyz;
  bool no_attr = all(v.nor == 0.0);
  float facing = no_attr ? no_nor_facing : dot(wnor, V);

  float4 clip_pos = drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(wpos, 1.0)));
  float facing_ratio = saturate(1.0 - facing * facing);
  float flip = facing < 0.0 ? -1.0 : 1.0;
  float curvature = 1.0 - v.wd * 0.75;
  float3 wofs = drw_mat_mul3(view_buf, DRW_VIEWMAT, wnor * (facing_ratio * curvature * flip));
  /* Push vertex half a pixel (maximum) in normal direction. */
  clip_pos.xy += wofs.xy * uniform_buf__97.zw * clip_pos.w;
  /* Push the vertex towards the camera. Helps a bit. */
  clip_pos.z -= facing_ratio * curvature * 1.0e-6 * clip_pos.w;
  clip_pos.z -= ndc_offset_factor * 0.5;
  o.position = gpu_position(clip_pos);

  float3 rim_col, wire_col;
  if (color_type == V3D_SHADING_OBJECT_COLOR || color_type == V3D_SHADING_RANDOM_COLOR) {
    rim_col = color_type == V3D_SHADING_OBJECT_COLOR ? ob_info[0].rgb * 0.5 :
                                                       hsv_to_rgb(float3(ob_info[1].z, 0.75, 0.8));
    wire_col = rim_col;
    if (is_selected > 0.5 && use_coloring > 0.5) {
      /* "Normalize" color. */
      wire_col += 1e-4;
      wire_col *= 0.5 / max(wire_col.x, max(wire_col.y, wire_col.z));
      rim_col += 0.75;
    }
    else {
      rim_col *= 0.5;
      wire_col += 0.5;
    }
  }
  else {
    float4 select_col = is_transform > 0.5 ? TH_TRANSFORM :
                                             (is_active > 0.5 ? TH_ACTIVE_OBJECT : TH_OBJECT_SELECT);
    bool colored = is_from_set < 0.5 && is_selected > 0.5 && use_coloring > 0.5;
    rim_col = colored ? select_col.rgb : TH_WIRE.rgb;
    wire_col = (is_from_set > 0.5 || colored) ? TH_WIRE.rgb : TH_BACKGROUND.rgb;
  }

  facing = saturate(abs(facing));
  /* Do interpolation in a non-linear space to have a better visual result. */
  rim_col = pow(abs(rim_col), 1.0 / 2.2);
  wire_col = pow(abs(wire_col), 1.0 / 2.2);
  float3 final_front_col = lerp(rim_col, wire_col, 0.35);
  float3 color = pow(abs(lerp(rim_col, final_front_col, facing)), 2.2);
  o.color = saturate(float4(color * wire_opacity, wire_opacity));
  /* Cull flat edges below threshold. */
  o.visible = (no_attr || v.wd <= wire_step_param) ? 1.0 : 0.0;
#ifdef SELECT
  o.select_data = select_data_get(select_custom_id.x, o.position);
#endif
  return o;
}

struct FragOut {
  float4 color : COLOR0;
  float4 line_output : COLOR1;
};

FragOut frag(VertOut v)
{
  FragOut o;
  clip(v.visible - 0.5);
  o.color = v.color;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
#ifdef SELECT
  o.color = select_output(v.select_data);
#endif
  return o;
}
