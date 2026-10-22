/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Envelope bones and distances (`overlay_armature_envelope_solid`). The envelope outlines aren't
 * supported.
 */

#include "overlay_armature_common.hlsl"

/* BoneEnvelopeData: head_sphere, tail_sphere, bone_color_and_wire_width, state_color, x_axis. */
// d3d9-instance envelope_data 0 80 0 instance ffffffffffffffffffff
float4 envelope_data[5];
float is_distance;

struct VertIn {
  float3 pos : TEXCOORD0;
};

struct VertOut {
  float4 position : POSITION;
  float4 bone_color : TEXCOORD0;
  float4 state_color : TEXCOORD1;
  float3 view_normal : TEXCOORD2;
  SELECT_VARYING
};

VertOut vert(VertIn v)
{
  VertOut o;
  float4 head_sphere = envelope_data[0];
  float4 tail_sphere = envelope_data[1];
  float3 bone_vec = tail_sphere.xyz - head_sphere.xyz;
  float bone_len = max(1e-8, length(bone_vec));
  float3 y_axis = bone_vec / bone_len;
  float3 z_axis = normalize(cross(envelope_data[4].xyz, -y_axis));
  /* Can't trust the x axis to be orthogonal. */
  float3 x_axis = cross(y_axis, z_axis);

  /* In bone space. */
  float3 sp = v.pos;
  bool is_head = v.pos.z < 0.0;
  sp *= is_head ? head_sphere.w : tail_sphere.w;
  sp.z += is_head ? 0.0 : bone_len;
  /* Convert to world space. */
  float3 ws_P = x_axis * sp.x + y_axis * sp.z + z_axis * sp.y + head_sphere.xyz;
  float3 nor = x_axis * v.pos.x + y_axis * v.pos.z + z_axis * v.pos.y;
  o.view_normal = drw_mat_mul3(view_buf, DRW_VIEWMAT, nor);
  o.bone_color = float4(envelope_data[2].rgb, alpha);
  o.state_color = float4(envelope_data[3].rgb, 0.0);
  o.position = gpu_position(view_to_clip(world_to_view(ws_P)));
#ifdef SELECT
  o.select_data = select_data_get(select_instance_id.x, o.position);
#endif
  return o;
}

FragOut frag(VertOut v)
{
  FragOut o;
  float n = normalize(v.view_normal).z;
  if (is_distance > 0.5) {
    n = 1.0 - saturate(-n);
    o.color = float4(1.0, 1.0, 1.0, 0.33 * alpha) * n;
  }
  else {
    /* Smooth lighting factor. */
    const float s = 0.2;
    float fac = saturate(n * (1.0 - s) + s);
    o.color = float4(lerp(v.state_color.rgb, v.bone_color.rgb, fac * fac), alpha);
  }
  o.line_output = float4(0.0, 0.0, 0.0, 0.0);
#ifdef SELECT
  o.color = select_output(v.select_data);
#endif
  return o;
}
