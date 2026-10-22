/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Selection variants of the overlay shaders (define SELECT, the `_selectable` create infos). The
 * fragments write their select id and depth instead of colors, the backend applies
 * `select_id_output()` (`select_lib.glsl`) on the CPU, see `D3D9Context::select_draw_begin`.
 *
 * Sources of the select id: the custom id of the draw (`drw_custom_id()`, select passes use
 * custom ids), `in_select_buf` (SELECT_ID_IN, slot 5) per instance or per vertex.
 */

#ifdef SELECT
// d3d9-select-output

/* Select id, clip space depth and W of the vertex. */
#  define SELECT_VARYING float3 select_data : TEXCOORD7;

float3 select_data_get(float select_id, float4 position)
{
  return float3(select_id, position.z, position.w);
}

/* The id is offset by one: 0 is the cleared background. */
float4 select_output(float3 select_data)
{
  return float4(select_data.x + 1.0, select_data.y / select_data.z, 0.0, 1.0);
}

/* `in_select_buf` value read per vertex as a D3DCOLOR (`d3d9-vertex-pull 5 4 <location> c`). */
float select_id_from_color(float4 c)
{
  return dot(floor(c * 255.0 + 0.5), float4(65536.0, 256.0, 1.0, 16777216.0));
}
#else
#  define SELECT_VARYING
#endif
