/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * UI widget (round box with outline, emboss and triangles), see `gpu_shader_2D_widget_base.bsl.hh`.
 * Defines: INSTANCED (parameters of up to 6 widgets, indexed by the instance).
 *
 * SM2.0 differences: no derivatives (the aspect ratio correction is dropped, the minimal line width
 * comes from the projection), the checker board uses the interpolated position.
 */

#include "d3d9_common.hlsl"

float4x4 ModelViewProjectionMatrix;
float3 checkerColorAndSize;
#ifdef INSTANCED
float4 parameters_inst[72];
#  define PARAM(i) parameters_inst[base + i]
#else
float4 parameters[12];
#  define PARAM(i) parameters[i]
#endif

struct VertIn {
  float vertex_id : TEXCOORD15;
};

struct VertOut {
  float4 position : POSITION;
  float4 uv_size : TEXCOORD0; /* uv_interp, out_rect_size */
  float4 border_color : TEXCOORD1;
  float4 emboss_color : TEXCOORD2;
  float4 corner_radii : TEXCOORD3;
  float4 inner_color : TEXCOORD4;
  float4 misc : TEXCOORD5; /* but_co, discard_fac, line_width, pixel size */
  float2 frag_co : TEXCOORD6;
};

/* Corner tables of the triangle shapes, indexed with a one-hot vector of the corner: indexing
 * static arrays would copy them to temporary registers, vs_2_0 only has 12. */
float2 arrow_pos(float4 w)
{
  return float2(dot(w, float4(0.0, 0.6, -0.6, 0.0)), dot(w, float4(0.6, 0.0, 0.0, -0.6)));
}
float2 arrow_uvs(float4 w)
{
  return float2(dot(w, float4(0.0, 0.85, 0.0, 0.0)), dot(w, float4(0.85, 0.85, 0.0, 0.85)));
}
float2 point_pos(float4 w)
{
  return float2(dot(w, float4(-1.0, -1.0, 1.0, 1.0)), dot(w, float4(-1.0, 1.0, -1.0, 1.0)));
}
float2 point_uvs(float4 w)
{
  return float2(dot(w, float4(0.0, 0.0, 1.0, 1.0)), dot(w, float4(0.0, 1.0, 0.0, 1.0)));
}

VertOut vert(VertIn v)
{
#ifdef INSTANCED
  int base = int(gpu_InstanceID) * 12;
#endif
  /* Read at the point of use: preloading all parameters exceeds the temporary registers. */
#define recti PARAM(0)
#define rect PARAM(1)
#define rads PARAM(2).y
#define round_corners PARAM(3)
#define color_inner1 PARAM(4)
#define color_inner2 PARAM(5)
#define color_edge PARAM(6)
#define color_emboss PARAM(7)
#define color_tria PARAM(8)
#define tria_centers PARAM(9)
#define p10 PARAM(10) /* tria1_size, tria2_size, shade_dir, alpha_discard */
#define tria_type PARAM(11).x

  /* Everything is computed without branches (vs_2_0 has no dynamic flow control, flattened
   * branches keep both sides alive and run out of the 12 temporary registers). */
  float vert_id = v.vertex_id;
  float is_tria = vert_id > 3.5;

  /* ---- Triangle shapes: accumulate the selected shape, one-hot over the type. ---- */
  float vidx = floor(fmod(vert_id + 0.5, 4.0));
  float4 w = float4(vidx == 0.0, vidx == 1.0, vidx == 2.0, vidx == 3.0);
  /* `min(vidx, 2)`: the third corner is repeated. */
  float4 w3 = float4(w.x, w.y, w.z + w.w, 0.0);
  float tria2 = vert_id > 7.5;
  float type = tria_type;

  float4 pu = float4(0.0, 0.0, 0.0, 0.0); /* pos, uv */
  float4 rc = float4(0.0, 0.0, 0.01, 1.0); /* rect size, corner radius, size factor */
  float t;
  float2 p;

  /* ROUNDBOX_TRIA_ARROWS */
  t = (type == 1.0);
  pu += t * float4(arrow_pos(w), arrow_uvs(w) - float2(0.05, 0.63));
  rc += t * float4(0.74, 0.17, 0.07, 0.0);
  /* ROUNDBOX_TRIA_SCROLL */
  t = (type == 2.0);
  pu += t * float4(point_pos(w), point_uvs(w));
  rc += t * float4(1.0, 1.0, 0.49, 0.0);
  /* ROUNDBOX_TRIA_MENU */
  t = (type == 3.0);
  p = arrow_pos(w) * (1.0 - tria2);
  pu += t * float4(p.y - 0.05, -p.x, arrow_uvs(w) - float2(0.05, 0.63));
  rc += t * float4(0.74, 0.17, 0.0, -0.2);
  /* ROUNDBOX_TRIA_CHECK */
  t = (type == 4.0);
  p = arrow_pos(w3);
  p.y = lerp(p.y, -p.y, tria2);
  p = p.x * float2(0.0872, -0.996) + p.y * float2(0.996, 0.0872) + float2(-0.1, 0.2);
  pu += t * float4(p, arrow_uvs(w) - lerp(float2(0.08, 0.65), float2(0.4, 0.65), tria2));
  rc += t * float4(0.74, 0.14, 0.0, 0.7);
  float use_tria1 = t;
  /* ROUNDBOX_TRIA_HOLD_ACTION_ARROW */
  t = (type == 5.0);
  p = arrow_pos(w3) * (1.0 - tria2);
  p = p.x * float2(0.707, 0.707) + p.y * float2(-0.707, 0.707) + float2(-1.7, 2.4);
  pu += t * float4(p, arrow_uvs(w) - float2(0.05, 0.05));
  rc += t * float4(0.75, 0.75, 0.0, -0.6);
  /* ROUNDBOX_TRIA_DASH */
  t = (type == 6.0);
  pu += t * float4(point_pos(w), point_uvs(w) - float2(0.2, 0.45));
  rc += t * float4(0.6, 0.1, 0.0, 0.0);

  /* The second triangle has a negative size (mirrored), except for the check mark. */
  float second = tria2 * (1.0 - use_tria1);
  float size = lerp(p10.x, -p10.y, second) * rc.w;
  float2 center = lerp(tria_centers.xy, tria_centers.zw, second);
  float abs_size = abs(size);
  float4 tria_pu = float4(pu.xy * size + center, pu.zw * abs_size);
  float3 tria_rc = rc.xyz * abs_size;

  /* ---- Widget rectangle. ---- */
  float line_width = abs(rect.x - recti.x);
  /* Offset to avoid losing pixels (mimics conservative rasterization). */
  float right = vert_id > 1.5;
  float top = frac(vert_id * 0.5) > 0.25;
  float2 pos = float2(lerp(rect.x - 0.5, rect.y + 0.5, right),
                      lerp(rect.z - line_width - 0.5, rect.w + 0.5, top));
  float2 rect_size = rect.yw - rect.xz;
  float2 uv_interp = pos - rect.xz;
  /* Guard divisions: vs_2_0 selects with `lerp`, a NaN on the unused side still propagates. */
  float2 uv = uv_interp / max(abs(rect_size), 1e-5);
  float fac = saturate((p10.z > 0.0) ? uv.y : uv.x);
  float alpha_check = p10.w < 0.0;
  float4 inner_color = lerp(lerp(color_inner2, color_inner1, fac), color_inner1, alpha_check);
  float but_co = lerp(-abs(uv.x), uv.x, alpha_check);

  /* ---- Select. ---- */
  VertOut o;
  pos = lerp(pos, tria_pu.xy, is_tria);
  o.uv_size = lerp(float4(uv_interp, rect_size), float4(tria_pu.zw, tria_rc.xy), is_tria);
  o.corner_radii = lerp(rads * round_corners, tria_rc.zzzz, is_tria);
  float4 tria_color = color_tria * float4(color_tria.aaa, 1.0);
  o.inner_color = lerp(inner_color, tria_color, is_tria);
  o.border_color = color_edge * float4(color_edge.aaa, 1.0) * (1.0 - is_tria);
  o.emboss_color = color_emboss * float4(color_emboss.aaa, 1.0) * (1.0 - is_tria);
  o.misc = float4(lerp(but_co, -2.0, is_tria),
                  abs(p10.w),
                  line_width * (1.0 - is_tria),
                  gpu_pixel_size_y(ModelViewProjectionMatrix));

  float4 clip_pos = mul(ModelViewProjectionMatrix, float4(pos, 0.0, 1.0));
  o.position = gpu_position(clip_pos);
  o.frag_co = gpu_frag_coord(clip_pos);
  return o;
}

float4 frag(VertOut v) : COLOR0
{
  float but_co = v.misc.x;
  float discard_fac = v.misc.y;
  clip(discard_fac - min(1.0, -but_co));

  float2 uv = v.uv_size.xy;
  float2 rect_size = v.uv_size.zw;
  bool upper_half = uv.y > rect_size.y * 0.5;
  bool right_half = uv.x > rect_size.x * 0.5;

  float2 uv_sdf = uv;
  if (right_half) {
    uv_sdf.x = rect_size.x - uv_sdf.x;
  }
  float corner_rad;
  if (upper_half) {
    uv_sdf.y = rect_size.y - uv_sdf.y;
    corner_rad = right_half ? v.corner_radii.z : v.corner_radii.w;
  }
  else {
    corner_rad = right_half ? v.corner_radii.y : v.corner_radii.x;
  }

  /* Fade emboss at the border. */
  float emboss_size = upper_half ? 0.0 : saturate(uv_sdf.x / max(corner_rad, 1e-5));

  /* Signed distance field from the corner (in pixel). */
  uv_sdf -= corner_rad;
  float inner_sdf = max(0.0, min(uv_sdf.x, uv_sdf.y));
  float outer_sdf = -length(min(uv_sdf, 0.0));
  float sdf = inner_sdf + outer_sdf + corner_rad;

  float line_width = (v.misc.z > 0.0) ? max(v.misc.w, v.misc.z) : 0.0;

  /* Linear ramps instead of smooth-steps: same coverage, fewer instructions. */
  float3 masks = saturate(float3(sdf, sdf - line_width, sdf + line_width * emboss_size) + 0.5);
  masks.zx = max(0.0, masks.zx - masks.xy);

  float4 color;
  if (but_co > 0.0) {
    /* Alpha checker widget. */
    if (but_co > 0.5) {
      float size = checkerColorAndSize.z;
      float2 phase = fmod(v.frag_co, size * 2.0);
      float checker = ((phase.x > size) != (phase.y > size)) ? checkerColorAndSize.x :
                                                              checkerColorAndSize.y;
      color = lerp(float4(checker, checker, checker, 1.0), v.inner_color, v.inner_color.a);
    }
    else {
      color = v.inner_color;
    }
    color.a = 1.0;
  }
  else {
    color = v.inner_color * float4(v.inner_color.aaa, 1.0);
  }
  color *= masks.y;
  color += masks.x * v.border_color;
  color += masks.z * v.emboss_color;

  /* Un-pre-multiply because the blend equation is already doing the multiplication. */
  color.rgb /= max(color.a, 1e-6);
  return color;
}
