/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "kernel/geom/object.h"
#include "kernel/globals.h"

CCL_NAMESPACE_BEGIN

// TODO(sergey): Look into avoid use of full Transform and use 3x3 matrix and
// 3-vector which might be faster.
ccl_device_forceinline Transform bvh_unaligned_node_fetch_space(KernelGlobals kg,
                                                                const int node_addr,
                                                                const int child)
{
  Transform space;
  const int child_addr = node_addr + child * 3;
  space.x = kernel_data_fetch(bvh_nodes, child_addr + 1);
  space.y = kernel_data_fetch(bvh_nodes, child_addr + 2);
  space.z = kernel_data_fetch(bvh_nodes, child_addr + 3);
  return space;
}

ccl_device_forceinline int bvh_aligned_node_intersect(KernelGlobals kg,
                                                      const float3 P,
                                                      const float3 idir,
                                                      const float tmin,
                                                      const float tmax,
                                                      const int node_addr,
                                                      const uint visibility,
                                                      float dist[2])
{

  /* fetch node data */
#ifdef __VISIBILITY_FLAG__
  float4 cnodes = kernel_data_fetch(bvh_nodes, node_addr + 0);
#endif
  float4 node0 = kernel_data_fetch(bvh_nodes, node_addr + 1);
  float4 node1 = kernel_data_fetch(bvh_nodes, node_addr + 2);
  float4 node2 = kernel_data_fetch(bvh_nodes, node_addr + 3);

  /* intersect ray against child nodes */
  float c0lox = (node0.x - P.x) * idir.x;
  float c0hix = (node0.z - P.x) * idir.x;
  float c0loy = (node1.x - P.y) * idir.y;
  float c0hiy = (node1.z - P.y) * idir.y;
  float c0loz = (node2.x - P.z) * idir.z;
  float c0hiz = (node2.z - P.z) * idir.z;
  float c0min = max4(tmin, min(c0lox, c0hix), min(c0loy, c0hiy), min(c0loz, c0hiz));
  float c0max = min4(tmax, max(c0lox, c0hix), max(c0loy, c0hiy), max(c0loz, c0hiz));

  float c1lox = (node0.y - P.x) * idir.x;
  float c1hix = (node0.w - P.x) * idir.x;
  float c1loy = (node1.y - P.y) * idir.y;
  float c1hiy = (node1.w - P.y) * idir.y;
  float c1loz = (node2.y - P.z) * idir.z;
  float c1hiz = (node2.w - P.z) * idir.z;
  float c1min = max4(tmin, min(c1lox, c1hix), min(c1loy, c1hiy), min(c1loz, c1hiz));
  float c1max = min4(tmax, max(c1lox, c1hix), max(c1loy, c1hiy), max(c1loz, c1hiz));

  dist[0] = c0min;
  dist[1] = c1min;

#ifdef __VISIBILITY_FLAG__
  /* this visibility test gives a 5% performance hit, how to solve? */
  return (((c0max >= c0min) && (__float_as_uint(cnodes.x) & visibility)) ? 1 : 0) |
         (((c1max >= c1min) && (__float_as_uint(cnodes.y) & visibility)) ? 2 : 0);
#else
  return ((c0max >= c0min) ? 1 : 0) | ((c1max >= c1min) ? 2 : 0);
#endif
}

#if defined(__KERNEL_SSE2__) && !defined(__KERNEL_NEON__)
/* Test both children of a node at once with SSE, like Cycles did before Embree was required on the
 * CPU (the 32-bit x86 build has no Embree). The ray is prepared once (#BVHNodeRay): per axis the
 * bounds are ordered near to far along the ray, and the far distances are negated so that one
 * `max()` gives the entry distances (lanes 0, 1) and the negated exit distances (lanes 2, 3). The
 * results are the same as the scalar test's. */
#  define __BVH_NODE_SSE__

struct BVHNodeRay {
  /* Ray origin per axis, in all lanes. */
  __m128 P[3];
  /* Inverse direction per axis, negated in lanes 2 and 3. */
  __m128 idir[3];
  /* The direction is negative: the upper bounds are the near ones. */
  bool swap[3];
};

ccl_device_forceinline void bvh_node_ray_init(ccl_private BVHNodeRay *ray,
                                              const float3 P,
                                              const float3 idir)
{
  const float origin[3] = {P.x, P.y, P.z};
  const float inverse_dir[3] = {idir.x, idir.y, idir.z};
  for (int axis = 0; axis < 3; axis++) {
    const float d = inverse_dir[axis];
    ray->P[axis] = _mm_set1_ps(origin[axis]);
    ray->idir[axis] = _mm_setr_ps(d, d, -d, -d);
    ray->swap[axis] = d < 0.0f;
  }
}

ccl_device_forceinline int bvh_aligned_node_intersect(KernelGlobals kg,
                                                      const float3 /*P*/,
                                                      const ccl_private BVHNodeRay &ray,
                                                      const float tmin,
                                                      const float tmax,
                                                      const int node_addr,
                                                      const uint visibility,
                                                      float dist[2])
{
  /* Bounds per axis: lower of child 0 and 1, upper of child 0 and 1. */
  const __m128 bounds[3] = {kernel_data_fetch(bvh_nodes, node_addr + 1).m128,
                            kernel_data_fetch(bvh_nodes, node_addr + 2).m128,
                            kernel_data_fetch(bvh_nodes, node_addr + 3).m128};

  __m128 t = _mm_setr_ps(tmin, tmin, -tmax, -tmax);
  for (int axis = 0; axis < 3; axis++) {
    const __m128 near_far = ray.swap[axis] ?
                                _mm_shuffle_ps(bounds[axis], bounds[axis], _MM_SHUFFLE(1, 0, 3, 2)) :
                                bounds[axis];
    t = _mm_max_ps(t, _mm_mul_ps(_mm_sub_ps(near_far, ray.P[axis]), ray.idir[axis]));
  }
  /* Entry distance of both children, exit distance of both children. */
  const __m128 entry_exit = _mm_xor_ps(
      t, _mm_castsi128_ps(_mm_setr_epi32(0, 0, int(0x80000000), int(0x80000000))));
  const __m128 exit_entry = _mm_shuffle_ps(entry_exit, entry_exit, _MM_SHUFFLE(1, 0, 3, 2));
  int mask = _mm_movemask_ps(_mm_cmple_ps(entry_exit, exit_entry)) & 3;

  dist[0] = _mm_cvtss_f32(entry_exit);
  dist[1] = _mm_cvtss_f32(_mm_shuffle_ps(entry_exit, entry_exit, _MM_SHUFFLE(1, 1, 1, 1)));

#  ifdef __VISIBILITY_FLAG__
  const float4 cnodes = kernel_data_fetch(bvh_nodes, node_addr + 0);
  mask &= ((__float_as_uint(cnodes.x) & visibility) ? 1 : 0) |
          ((__float_as_uint(cnodes.y) & visibility) ? 2 : 0);
#  endif
  return mask;
}

/* The traversal functions pass #BVH_NODE_RAY to the node tests, and update it when they transform
 * the ray (`P`, `idir`) into or out of an instance. */
#  define BVH_NODE_RAY node_ray
#  define BVH_NODE_RAY_DECLARE() \
    BVHNodeRay node_ray; \
    bvh_node_ray_init(&node_ray, P, idir)
#  define BVH_NODE_RAY_UPDATE() bvh_node_ray_init(&node_ray, P, idir)
#else
#  define BVH_NODE_RAY idir
#  define BVH_NODE_RAY_DECLARE()
#  define BVH_NODE_RAY_UPDATE()
#endif

ccl_device_forceinline bool bvh_unaligned_node_intersect_child(KernelGlobals kg,
                                                               const float3 P,
                                                               const float3 dir,
                                                               const float tmin,
                                                               const float tmax,
                                                               const int node_addr,
                                                               const int child,
                                                               float dist[2])
{
  Transform space = bvh_unaligned_node_fetch_space(kg, node_addr, child);
  float3 aligned_dir = transform_direction(&space, dir);
  float3 aligned_P = transform_point(&space, P);
  float3 nrdir = -bvh_inverse_direction(aligned_dir);
  float3 lower_xyz = aligned_P * nrdir;
  float3 upper_xyz = lower_xyz - nrdir;
  const float near_x = min(lower_xyz.x, upper_xyz.x);
  const float near_y = min(lower_xyz.y, upper_xyz.y);
  const float near_z = min(lower_xyz.z, upper_xyz.z);
  const float far_x = max(lower_xyz.x, upper_xyz.x);
  const float far_y = max(lower_xyz.y, upper_xyz.y);
  const float far_z = max(lower_xyz.z, upper_xyz.z);
  const float tnear = max4(tmin, near_x, near_y, near_z);
  const float tfar = min4(tmax, far_x, far_y, far_z);
  *dist = tnear;
  return tnear <= tfar;
}

ccl_device_forceinline int bvh_unaligned_node_intersect(KernelGlobals kg,
                                                        const float3 P,
                                                        const float3 dir,
                                                        const float tmin,
                                                        const float tmax,
                                                        const int node_addr,
                                                        const uint visibility,
                                                        float dist[2])
{
  int mask = 0;
#ifdef __VISIBILITY_FLAG__
  float4 cnodes = kernel_data_fetch(bvh_nodes, node_addr + 0);
#endif
  if (bvh_unaligned_node_intersect_child(kg, P, dir, tmin, tmax, node_addr, 0, &dist[0])) {
#ifdef __VISIBILITY_FLAG__
    if ((__float_as_uint(cnodes.x) & visibility))
#endif
    {
      mask |= 1;
    }
  }
  if (bvh_unaligned_node_intersect_child(kg, P, dir, tmin, tmax, node_addr, 1, &dist[1])) {
#ifdef __VISIBILITY_FLAG__
    if ((__float_as_uint(cnodes.y) & visibility))
#endif
    {
      mask |= 2;
    }
  }
  return mask;
}

#ifdef __BVH_NODE_SSE__
ccl_device_forceinline int bvh_node_intersect(KernelGlobals kg,
                                              const float3 P,
                                              const float3 dir,
                                              const ccl_private BVHNodeRay &ray,
                                              const float tmin,
                                              const float tmax,
                                              const int node_addr,
                                              const uint visibility,
                                              float dist[2])
{
  float4 node = kernel_data_fetch(bvh_nodes, node_addr);
  if (__float_as_uint(node.x) & PATH_RAY_VISIBILITY_NODE_UNALIGNED) {
    return bvh_unaligned_node_intersect(kg, P, dir, tmin, tmax, node_addr, visibility, dist);
  }
  return bvh_aligned_node_intersect(kg, P, ray, tmin, tmax, node_addr, visibility, dist);
}
#endif

ccl_device_forceinline int bvh_node_intersect(KernelGlobals kg,
                                              const float3 P,
                                              const float3 dir,
                                              const float3 idir,
                                              const float tmin,
                                              const float tmax,
                                              const int node_addr,
                                              const uint visibility,
                                              float dist[2])
{
  float4 node = kernel_data_fetch(bvh_nodes, node_addr);
  if (__float_as_uint(node.x) & PATH_RAY_VISIBILITY_NODE_UNALIGNED) {
    return bvh_unaligned_node_intersect(kg, P, dir, tmin, tmax, node_addr, visibility, dist);
  }
  return bvh_aligned_node_intersect(kg, P, idir, tmin, tmax, node_addr, visibility, dist);
}

CCL_NAMESPACE_END
