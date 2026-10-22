/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Bone joints (`overlay_armature_sphere_*`): camera facing discs. Defines: SOLID, OUTLINE.
 *
 * D3D9 differences: the solid sphere isn't ray traced, its lighting is derived from the distance
 * to the disc center and its depth is the one of the disc (no `gl_FragDepth`).
 */

#include "overlay_armature_common.hlsl"

// d3d9-instance bone_data 0 64 0 instance ffffffffffffffff
float4 bone_data[4];

#define SPHERE_RADIUS 0.05

struct VertIn {
  float2 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  float4 color : TEXCOORD0;
#ifdef SOLID
  float4 state_color : TEXCOORD1;
  /* Position on the disc, 1 on the outline. */
  float2 disc : TEXCOORD2;
#endif
  SELECT_VARYING
};

VertOut vert(VertIn v)
{
  VertOut o;
  float4 columns[4] = {bone_data[0], bone_data[1], bone_data[2], bone_data[3]};
  BoneMatrix m = bone_matrix_unpack(columns);
  /* Model view matrix: columns a0 to a2 and translation, its inverse rows are r0 to r2 (up to
   * the determinant). */
  float3 a0 = drw_mat_mul3(view_buf, DRW_VIEWMAT, m.x);
  float3 a1 = drw_mat_mul3(view_buf, DRW_VIEWMAT, m.y);
  float3 a2 = drw_mat_mul3(view_buf, DRW_VIEWMAT, m.z);
  float3 t = world_to_view(m.loc);
  float3 r0 = cross(a1, a2);
  float3 r1 = cross(a2, a0);
  float3 r2 = cross(a0, a1);
  float inv_det = 1.0 / dot(a0, r0);
  bool persp = is_persp();

  /* This is the local space camera ray (not normalize). In perspective mode it's also the
   * view-space position of the sphere center. */
  float3 cam_ray = persp ? t : float3(0.0, 0.0, -1.0);
  cam_ray = float3(dot(r0, cam_ray), dot(r1, cam_ray), dot(r2, cam_ray)) * inv_det;
  /* Sphere center distance from the camera (persp) in local space. */
  float cam_dist = length(cam_ray);
  /* Compute view aligned orthonormal space. The local view Y axis is the column 1 of the
   * inverse. */
  float3 z_axis = cam_ray / cam_dist;
  float3 view_y = float3(r0.y, r1.y, r2.y) * inv_det;
  float3 x_axis = normalize(cross(view_y, z_axis));
  float3 y_axis = cross(z_axis, x_axis);
#ifdef SOLID
  /* Offset to the front of the sphere. */
  float z_ofs = -SPHERE_RADIUS - 1e-8;
#else
  float z_ofs = 0.0;
#endif
  if (persp) {
    /* The projected sphere radius can be bigger than the center disc. Compute the max angular
     * size and compensate by sliding the disc towards the camera and scale it accordingly. */
    float cos_b = SPHERE_RADIUS / cam_dist;
    float sin_b = sqrt(saturate(1.0 - cos_b * cos_b));
#ifdef SOLID
    /* Choose the nearest circle with the same angular size. */
    float minor = cam_dist - SPHERE_RADIUS;
    float major = cam_dist - cos_b * SPHERE_RADIUS;
    sin_b *= minor / major;
#else
    z_ofs = -SPHERE_RADIUS * cos_b;
#endif
    x_axis *= sin_b;
    y_axis *= sin_b;
  }
  /* Camera oriented position (but still in local space). */
  float3 cam_pos = x_axis * v.pos.x + y_axis * v.pos.y + z_axis * z_ofs;
  float3 vs_P = a0 * cam_pos.x + a1 * cam_pos.y + a2 * cam_pos.z + t;
  float4 hs_P = view_to_clip(vs_P);

#ifdef SOLID
  o.color = float4(m.bone_color.rgb, alpha);
  o.state_color = float4(m.state_color.rgb, 0.0);
  o.disc = v.pos / SPHERE_RADIUS;
#else
  /* Offset away from the center to avoid overlap with solid shape. */
  float4 center = view_to_clip(t);
  float2 ofs_dir = normalize(clip_to_screen(hs_P) - clip_to_screen(center) + 1e-8);
  hs_P.xy += ofs_dir * size_viewport_inv * hs_P.w;
  o.color = float4(m.bone_color.rgb, alpha);
#endif
  o.position = gpu_position(hs_P);
#ifdef SELECT
  o.select_data = select_data_get(select_instance_id.x, o.position);
#endif
  return o;
}

FragOut frag(VertOut v)
{
  FragOut o;
  o.color = v.color;
#ifdef SOLID
  /* Lighting from the view axis: the sphere normal facing the camera is the disc height. */
  float n_dot_l = sqrt(saturate(1.0 - dot(v.disc, v.disc)));
  /* Smooth lighting factor. */
  const float s = 0.2;
  float fac = saturate(n_dot_l * (1.0 - s) + s);
  o.color.rgb = lerp(v.state_color.rgb, v.color.rgb, fac * fac);
#endif
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
#ifdef SELECT
  o.color = select_output(v.select_data);
#endif
  return o;
}
