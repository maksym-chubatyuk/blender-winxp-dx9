/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Octahedral, box and custom shape bones (`overlay_armature_shape_*`). Defines: SOLID, OUTLINE,
 * WIRE.
 *
 * D3D9 differences:
 * - SOLID: back faces are discarded from the facing of the flat triangle computed per vertex
 *   (ps_2_0 has no `gl_FrontFacing`).
 * - OUTLINE: the lines with adjacency are drawn as lines carrying the 4 positions (the backend
 *   expands them), silhouette edges are 1 pixel lines.
 * - WIRE: custom shape wires are 1 pixel lines, the bone arrows aren't supported.
 */

#include "overlay_armature_common.hlsl"

#if defined(OUTLINE) || defined(WIRE)
/* Shape positions are bound as slot 0. */
// d3d9-instance bone_data 1 64 0 instance ffffffffffffffff
#else
// d3d9-instance bone_data 0 64 0 instance ffffffffffffffff
#endif
float4 bone_data[4];

#ifdef OUTLINE
// d3d9-lines-adjacency
struct VertIn {
  float3 pos0 : TEXCOORD0;
  float3 pos1 : TEXCOORD1;
  float3 pos2 : TEXCOORD2;
  float3 pos3 : TEXCOORD3;
  /* 0: first vertex of the edge, 1: second. */
  float end : TEXCOORD4;
};
#elif defined(WIRE)
struct VertIn {
  float3 pos : TEXCOORD0;
};
#else
struct VertIn {
  float3 pos : TEXCOORD0;
  float3 nor : TEXCOORD1;
};
#endif

struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
#ifdef SOLID
  /* Negative for back faces. */
  float facing : TEXCOORD1;
#endif
  SELECT_VARYING
};

VertOut vert(VertIn v)
{
  VertOut o;
  float4 columns[4] = {bone_data[0], bone_data[1], bone_data[2], bone_data[3]};
  BoneMatrix m = bone_matrix_unpack(columns);
  const float det = bone_determinant(m);

#if defined(SOLID)
  /* Inverse transpose of the bone matrix (up to a positive scale) times the normal. */
  float3 normal = (cross(m.y, m.z) * v.nor.x + cross(m.z, m.x) * v.nor.y +
                   cross(m.x, m.y) * v.nor.z) *
                  sign(det);
  float3 vs_N = drw_mat_mul3(view_buf, DRW_VIEWMAT, normal);
  float3 vs_P = world_to_view(bone_point(m, v.pos));
  /* The facing of a flat triangle is the same for all its points. */
  o.facing = is_persp() ? -dot(vs_N, vs_P) : vs_N.z;
  vs_N = normalize(vs_N);

  /* Do lighting at an angle to avoid flat shading on front facing bone. */
  const float3 light = float3(0.1, 0.1, 0.8);
  float n = dot(vs_N, light);
  /* Smooth lighting factor. */
  const float s = 0.2;
  float fac = saturate(n * (1.0 - s) + s);
  o.color = float4(lerp(m.state_color.rgb, m.bone_color.rgb, fac * fac), alpha);
  o.position = gpu_position(view_to_clip(vs_P));

#elif defined(WIRE)
  float4 hs_P = view_to_clip(world_to_view(bone_point(m, v.pos)));
  o.color = float4(lerp(m.state_color.rgb, m.bone_color.rgb, 0.5), alpha);
  o.position = gpu_position(hs_P);

#else
  /* `geometry_main` of the GLSL, for the vertex `end` of the edge 1-2. */
  float3 vs_P0 = world_to_view(bone_point(m, v.pos0));
  float3 vs_P1 = world_to_view(bone_point(m, v.pos1));
  float3 vs_P2 = world_to_view(bone_point(m, v.pos2));
  float3 vs_P3 = world_to_view(bone_point(m, v.pos3));
  float4 hs_P0 = view_to_clip(vs_P0);
  float4 hs_P1 = view_to_clip(vs_P1);
  float4 hs_P2 = view_to_clip(vs_P2);
  float4 hs_P3 = view_to_clip(vs_P3);
  float2 ss_P0 = clip_to_screen(hs_P0);
  float2 ss_P1 = clip_to_screen(hs_P1);
  float2 ss_P2 = clip_to_screen(hs_P2);
  float2 ss_P3 = clip_to_screen(hs_P3);

  bool persp = is_persp();
  float3 view_vec = persp ? normalize(vs_P1) : float3(0.0, 0.0, -1.0);
  float3 v10 = vs_P0 - vs_P1;
  float3 v12 = vs_P2 - vs_P1;
  float3 v13 = vs_P3 - vs_P1;
  float3 n0 = cross(v12, v10);
  n0 *= 1.0 / max(length(n0), 1e-20);
  float3 n3 = cross(v13, v12);
  n3 *= 1.0 / max(length(n3), 1e-20);
  float fac0 = dot(view_vec, n0);
  float fac3 = dot(view_vec, n3);

  /* If one of the faces is perpendicular to the view, consider it an outline edge. If both
   * adjacent faces are facing the camera the same way, it isn't one. */
  bool outline = !(abs(fac0) > 1e-5 && abs(fac3) > 1e-5 && fac0 * fac3 > 0.0);
  n0 = det < 0.0 ? -n0 : n0;
  /* Don't outline if concave edge. */
  outline = outline && dot(n0, v13) <= 0.0001;

  float2 perp = normalize(ss_P2 - ss_P1 + 1e-8);
  float2 edge_dir = float2(-perp.y, perp.x);
  /* Take the farthest point to compute edge direction (avoid problems with point behind near
   * plane). If the chosen point is parallel to the edge in screen space, choose the other one. */
  float2 hidden_point;
  if (vs_P0.z < vs_P3.z) {
    hidden_point = abs(fac0) > 1e-5 ? ss_P0 : ss_P3;
  }
  else {
    hidden_point = abs(fac3) > 1e-5 ? ss_P3 : ss_P0;
  }
  float2 hidden_dir = normalize(hidden_point - ss_P1 + 1e-8);
  edge_dir *= dot(-hidden_dir, edge_dir) < 0.0 ? -1.0 : 1.0;

  bool second = v.end > 0.5;
  float4 hs_P = second ? hs_P2 : hs_P1;
  float2 offset = second ? edge_dir + perp : edge_dir - perp;
  /* Offset away from the center to avoid overlap with solid shape. */
  hs_P.xy += offset * size_viewport_inv * hs_P.w;
  /* Improve AA bleeding inside bone silhouette. */
  hs_P.z -= persp ? 1e-4 : 1e-6;
  o.color = float4(m.bone_color.rgb, alpha);
  o.position = outline ? gpu_position(hs_P) : DISCARDED_POSITION;
#endif

#ifdef SELECT
  o.select_data = select_data_get(select_instance_id.x, o.position);
#endif
  return o;
}

FragOut frag(VertOut v)
{
  FragOut o;
#ifdef SOLID
  /* Manual back-face culling: needed in X-ray and for inverted bone matrices. */
  clip(v.facing);
#endif
  o.color = v.color;
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
#ifdef SELECT
  o.color = select_output(v.select_data);
#endif
  return o;
}
