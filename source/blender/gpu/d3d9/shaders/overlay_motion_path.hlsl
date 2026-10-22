/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Motion paths (`overlay_motion_path_*`). Defines: LINE, POINT.
 *
 * D3D9 differences: the path is a 1 pixel line strip drawn from the points.
 */

#include "d3d9_common.hlsl"
#define DRW_NO_MODEL
#include "draw_common.hlsl"

float4 view_buf[12];
/* Theme colors: wire (0) to after_frame (85). */
float4 uniform_buf__0[86];
/* Theme sizes: pixel. */
float4 uniform_buf__94;
float3 custom_color_pre;
float3 custom_color_post;
float4x4 camera_object_persinv;

#define TH_VERT uniform_buf__0[12]
#define TH_VERT_SELECT uniform_buf__0[13]
#define TH_BACKGROUND uniform_buf__0[41]
#define TH_CURRENT_FRAME uniform_buf__0[83]
#define TH_BEFORE_FRAME uniform_buf__0[84]
#define TH_AFTER_FRAME uniform_buf__0[85]

#ifdef LINE
// d3d9-points-as-line-strip
float4 mpath_line_settings;
struct VertIn {
  float3 pos : TEXCOORD0;
  float vertex_id : TEXCOORD15;
};
struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
};
#else
// d3d9-point-sprite
float4 mpath_point_settings;
float show_key_frames;
struct VertIn {
  float3 pos : TEXCOORD0;
  float flag : TEXCOORD1;
  float vertex_id : TEXCOORD15;
};
struct VertOut {
  float4 position : POSITION;
  float point_size : PSIZE;
  float4 color : COLOR0;
};
#endif

VertOut vert(VertIn v)
{
  VertOut o;
  /* Optionally transform from view space to world space for screen space motion paths. */
  float4 p = mul(camera_object_persinv, float4(v.pos, 1.0));
  float3 ws_P = p.xyz / p.w;
  float4 hs_P = drw_mat_mul(
      view_buf, DRW_WINMAT, drw_mat_mul(view_buf, DRW_VIEWMAT, float4(ws_P, 1.0)));
  bool use_custom_color = custom_color_pre.x >= 0.0;
#ifdef LINE
  float frame_current = mpath_line_settings.x;
  float frame = v.vertex_id + mpath_line_settings.w;
  if (frame < frame_current) {
    o.color.rgb = use_custom_color ? custom_color_pre : TH_BEFORE_FRAME.rgb;
  }
  else if (frame > frame_current) {
    o.color.rgb = use_custom_color ? custom_color_post : TH_AFTER_FRAME.rgb;
  }
  else {
    o.color.rgb = TH_CURRENT_FRAME.rgb;
  }
  o.color.a = 1.0;
#else
  float pt_size = mpath_point_settings.x;
  float frame_current = mpath_point_settings.y;
  float frame = v.vertex_id + mpath_point_settings.z;
  float step_size = max(mpath_point_settings.w, 1.0);
  float point_size = pt_size + 2.0;
  o.color = use_custom_color ? float4(custom_color_pre, 1.0) : TH_VERT;
  /* Bias to reduce z fighting with the path. */
  hs_P.z -= 1e-4;
  if (fmod(v.vertex_id, step_size) < 0.5) {
    point_size = pt_size + 4.0;
  }
  /* Draw special dot where the current frame is. */
  if (frame == frame_current) {
    point_size = pt_size + 8.0;
    o.color = TH_CURRENT_FRAME;
    /* Bias more to get these on top of keyframes. */
    hs_P.z -= 1e-4;
  }
  else if (use_custom_color) {
    o.color = float4(frame < frame_current ? custom_color_pre : custom_color_post, 1.0);
  }
  /* Overrides the color to highlight points that are keyframes (MOTIONPATH_VERT_KEY). */
  if (show_key_frames > 0.5 && fmod(floor(v.flag / 2.0), 2.0) > 0.5) {
    point_size = pt_size + 5.0;
    o.color = TH_VERT_SELECT;
    /* Bias more to get these on top of regular points. */
    hs_P.z -= 1e-4;
  }
  o.point_size = point_size * uniform_buf__94.x;
#endif
  o.position = gpu_position(hs_P);
  return o;
}

#ifdef LINE
float4 frag(VertOut v) : COLOR0
{
  return v.color;
}
#else
float4 frag(VertOut v, float2 point_coord : TEXCOORD0) : COLOR0
{
  float2 centered = point_coord - 0.5;
  /* Round point with jagged edges. */
  clip(0.25 - dot(centered, centered));
  return v.color;
}
#endif
