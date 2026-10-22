/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Draw manager resources for the Direct3D 9 shaders (`draw_view.bsl.hh`, `draw_model.bsl.hh`).
 * Shaders using the object matrices also declare the resource id buffer:
 * `// d3d9-resource-id 11 <1, or 2 with custom ids>` (DRW_RESOURCE_ID_SLOT).
 */

/* First view of `view_buf` (DRW_VIEW_UBO_SLOT): viewmat, viewinv, winmat, wininv. Column major,
 * 4 registers each. Pixel shaders declare sub-ranges (`view_buf__8` ...) to save constants. */
#define DRW_VIEWMAT 0
#define DRW_VIEWINV 4
#define DRW_WINMAT 8
#define DRW_WININV 12

/* Column major matrix (4 registers from `m`) times vector. */
#define drw_mat_mul(buf, m, v) \
  (buf[(m)] * (v).x + buf[(m) + 1] * (v).y + buf[(m) + 2] * (v).z + buf[(m) + 3] * (v).w)
#define drw_mat_mul3(buf, m, v) \
  ((buf[(m)] * (v).x + buf[(m) + 1] * (v).y + buf[(m) + 2] * (v).z).xyz)

#ifndef DRW_NO_MODEL
/* Object matrices of the instance (`drw_matrix_buf`, DRW_OBJ_MAT_SLOT, `ObjectMatrices`). */
// d3d9-instance drw_model 10 128 0 res ffffffffffffffff
// d3d9-instance drw_model_inverse 10 128 64 res ffffffffffffffff
float4 drw_model[4];
float4 drw_model_inverse[4];

float3 drw_point_object_to_world(float3 p)
{
  return (drw_model[0] * p.x + drw_model[1] * p.y + drw_model[2] * p.z + drw_model[3]).xyz;
}

/* Transpose of the inverse, times the normal. */
float3 drw_normal_object_to_world(float3 n)
{
  return float3(dot(drw_model_inverse[0].xyz, n),
                dot(drw_model_inverse[1].xyz, n),
                dot(drw_model_inverse[2].xyz, n));
}
#endif

/* Full-screen triangle of 3 procedural vertices (`fullscreen_vertex`). */
float2 drw_fullscreen_pos(float vertex_id)
{
  return float2((vertex_id > 0.5 && vertex_id < 1.5) ? 3.0 : -1.0, vertex_id > 1.5 ? 3.0 : -1.0);
}
