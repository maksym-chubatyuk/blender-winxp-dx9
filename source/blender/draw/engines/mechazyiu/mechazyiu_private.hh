/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * MECHAZYIU: render engine for Shader Model 2.0 GPUs (the Direct3D 9 backend).
 *
 * Deferred shading, one pass per light, and many jittered samples averaged together: the
 * samples give the anti-aliasing and integrate the light shapes (area and soft lights). Each
 * sample is rendered in tiles sized to fit the free video memory, see #TilePlan.
 */

#pragma once

#include <array>
#include <memory>

#include "BLI_array.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "GPU_texture.hh"

namespace blender {

struct Image;
struct ImBuf;
struct Light;
struct Material;
struct Object;
struct Scene;
struct View3D;
struct World;

namespace gpu {
class Texture;
}

namespace mechazyiu {

/* -------------------------------------------------------------------- */
/** \name Materials
 * \{ */

/** An image read by a material: one of its channels (scalar maps) or its color. */
struct MapTexture {
  gpu::Texture **texture = nullptr;
  GPUSamplerState sampler = GPUSamplerState::default_sampler();
  /** 0 to 3: red to alpha. */
  int channel = 0;

  bool valid() const
  {
    return texture && *texture;
  }
};

/** Surface parameters of a material, read from its node tree (Principled BSDF and the common
 * simple shaders). Nodes the engine doesn't handle use their input values. */
struct MaterialData {
  float3 base_color = float3(0.8f);
  float alpha = 1.0f;
  float metallic = 0.0f;
  float roughness = 0.5f;
  /** Reflectance of the dielectric part at normal incidence. */
  float f0 = 0.04f;
  float3 emission = float3(0.0f);
  /** Image multiplying the base color (the image texture linked to it). */
  gpu::Texture **texture = nullptr;
  GPUSamplerState sampler = GPUSamplerState::default_sampler();
  /** The alpha of the image multiplies `alpha` (the image's alpha output is linked to it). */
  bool texture_alpha = false;
  /** Images of the other inputs (tangent space normal map, roughness, metallic, the coverage of
   * cutouts: the alpha input linked to an image). */
  MapTexture normal_map;
  float normal_strength = 1.0f;
  MapTexture roughness_map;
  MapTexture metallic_map;
  MapTexture coverage_map;

  /** More than the base color image: the G-buffer variant reading every map. */
  bool has_maps() const
  {
    return normal_map.valid() || roughness_map.valid() || metallic_map.valid() ||
           (coverage_map.valid() && coverage_map.texture != texture);
  }
  bool backface_culling = false;
  /** Material render method "Blended". */
  bool blended = false;

  /**
   * Partly transparent: sorted, drawn over the lit image (also for the "Dithered" render method,
   * dithering needs far more samples than this hardware can render).
   */
  bool transparent() const
  {
    return blended || alpha < 1.0f;
  }
  /** Image alpha cutouts: alpha tested at 0.5, like the games of the time. */
  bool cutout() const
  {
    return !transparent() && coverage_map.valid();
  }
};

MaterialData material_data_get(const Material *material);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lights
 * \{ */

enum class LightType { POINT = 0, SPOT, SUN, AREA };
constexpr int light_type_len = 4;

struct LightData {
  LightType type = LightType::POINT;
  /** World transform, the light points to -Z. */
  float4x4 object_to_world = float4x4::identity();
  /** Radiant intensity (W/sr, along the normal for area lights), or irradiance for suns. */
  float3 intensity = float3(0.0f);
  /** Point and spot radius, sun angle. */
  float radius = 0.0f;
  float2 area_size = float2(0.0f);
  bool area_round = false;
  float spot_cos_half = 0.0f;
  float spot_smooth = 0.0f;
  float diffuse_factor = 1.0f;
  float specular_factor = 1.0f;
  bool shadow = true;
  /** Distance where the light stops being computed. */
  float influence_radius = 0.0f;
};

LightData light_data_get(const Object &ob, float light_threshold);

/** Position (or direction toward a sun) for one sample: a point of the light's shape. */
float3 light_sample_position(const LightData &light, float2 rand);

/** \} */

/* -------------------------------------------------------------------- */
/** \name World
 * \{ */

/**
 * World lighting reduced to first order spherical harmonics (in world space): the irradiance
 * divided by pi is `sh0 + n.x * sh1[0] + n.y * sh1[1] + n.z * sh1[2]`.
 */
/** RGBM encoding of the environment cube: color = rgb * a * range. */
constexpr float environment_rgbm_range = 16.0f;

/** A cube map: RGBM encoded faces in the order and orientation of Direct3D cube maps. */
struct EnvironmentCube {
  int size = 0;
  /** Per mip level, the 6 faces of RGBA bytes (sizes halve). */
  Vector<Array<uchar>> levels;
};

/**
 * The environment for the reflections of increasing roughness (0, 1/3, 2/3: blurred more), and
 * its irradiance / pi (diffuse lighting, and the roughest reflections).
 */
struct EnvironmentMaps {
  std::array<EnvironmentCube, 4> cubes;
};

struct WorldLighting {
  /** Spherical harmonics in the environment's space. */
  float3 sh0 = float3(0.0f);
  float3 sh1[3] = {float3(0.0f), float3(0.0f), float3(0.0f)};
  /** Color of the background pixels (uniform worlds, or no visible environment). */
  float3 background = float3(0.0f);
  /** The environment image (not rotated, strength 1); null for uniform worlds. */
  std::shared_ptr<const EnvironmentMaps> maps;
  /** Multiplies the maps and the spherical harmonics. */
  float strength = 1.0f;
  /** Rotation of the environment around Z (the maps and spherical harmonics aren't rotated). */
  float rotation_z = 0.0f;
  /** The environment turns with the view (studio lights not in world space). */
  bool view_space = false;
  /** Visibility of the environment as background (studio lights) and its blur (EEVEE's). */
  float background_opacity = 1.0f;
  float background_blur = 0.0f;
};

/** Cubes of one texel per face, of `color`. */
std::shared_ptr<EnvironmentMaps> environment_maps_uniform(const float3 &color);
WorldLighting world_lighting_get(const World *world);
/** Material preview: the studio light of the viewport. */
WorldLighting world_lighting_studio_get(const View3D &v3d);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampling
 * \{ */

/** Radical inverse in the given base (Halton sequence), the sample index starts at 1. */
float halton(int index, int base);

/** \} */

}  // namespace mechazyiu
}  // namespace blender
