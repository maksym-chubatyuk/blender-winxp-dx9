/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * Scene data of MECHAZYIU: materials read from their node trees, lights, world lighting.
 */

#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "BKE_colorband.hh"
#include "BKE_image.hh"
#include "BKE_light.h"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_studiolight.h"

#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"
#include "DNA_world_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "NOD_shader.h"

#include "mechazyiu_private.hh"

namespace blender::mechazyiu {

/* -------------------------------------------------------------------- */
/** \name Node Tree Reading
 * \{ */

static const bNodeSocket *input_find(const bNode &node, StringRef identifier)
{
  for (const bNodeSocket *socket : node.input_sockets()) {
    if (socket->is_available() && StringRef(socket->identifier) == identifier) {
      return socket;
    }
  }
  return nullptr;
}

/** Output socket linked to an input, skipping reroutes and muted nodes. */
static const bNodeSocket *input_link_get(const bNodeSocket *socket)
{
  if (socket == nullptr) {
    return nullptr;
  }
  const Span<const bNodeSocket *> links = socket->logically_linked_sockets();
  return links.is_empty() ? nullptr : links.first();
}

static float input_float(const bNode &node, StringRef identifier, float fallback)
{
  const bNodeSocket *socket = input_find(node, identifier);
  if (socket == nullptr || socket->type != SOCK_FLOAT) {
    return fallback;
  }
  return socket->default_value_typed<bNodeSocketValueFloat>()->value;
}

/**
 * Color of an input: from an RGB node or a color ramp linked to it (at its factor, or its mean
 * color when the factor varies), or the input's own value.
 */
static float3 input_color(const bNode &node, StringRef identifier, float3 fallback)
{
  const bNodeSocket *socket = input_find(node, identifier);
  if (socket == nullptr || socket->type != SOCK_RGBA) {
    return fallback;
  }
  if (const bNodeSocket *from = input_link_get(socket)) {
    const bNode &from_node = from->owner_node();
    if (from_node.type_legacy == SH_NODE_RGB) {
      return float3(from->default_value_typed<bNodeSocketValueRGBA>()->value);
    }
    if (from_node.type_legacy == SH_NODE_VALTORGB && from_node.storage) {
      const ColorBand *ramp = static_cast<const ColorBand *>(from_node.storage);
      const bNodeSocket *fac = input_find(from_node, "Fac");
      float4 color;
      if (fac && !input_link_get(fac)) {
        BKE_colorband_evaluate(ramp, fac->default_value_typed<bNodeSocketValueFloat>()->value, color);
        return float3(color);
      }
      float3 sum(0.0f);
      for (int i = 0; i < 16; i++) {
        BKE_colorband_evaluate(ramp, (float(i) + 0.5f) / 16.0f, color);
        sum += float3(color);
      }
      return sum / 16.0f;
    }
  }
  return float3(socket->default_value_typed<bNodeSocketValueRGBA>()->value);
}

/** Image texture node linked to a color input. */
static const bNode *input_image_node(const bNode &node, StringRef identifier)
{
  const bNodeSocket *from = input_link_get(input_find(node, identifier));
  if (from == nullptr || from->owner_node().type_legacy != SH_NODE_TEX_IMAGE) {
    return nullptr;
  }
  const bNode &image_node = from->owner_node();
  const Image *image = reinterpret_cast<const Image *>(image_node.id);
  if (image == nullptr || image->source == IMA_SRC_TILED) {
    return nullptr;
  }
  return &image_node;
}

static void map_texture_set(MapTexture &map, const bNode &image_node)
{
  Image *image = reinterpret_cast<Image *>(image_node.id);
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(image_node.storage);
  ImageUser iuser = storage->iuser;
  map.texture = BKE_image_get_gpu_material_texture(image, &iuser, false).texture;

  GPUSamplerState &sampler = map.sampler;
  sampler = GPUSamplerState::default_sampler();
  const bool use_filter = storage->interpolation != SHD_INTERP_CLOSEST;
  sampler.set_filtering_flag_from_test(GPU_SAMPLER_FILTERING_LINEAR, use_filter);
  sampler.set_filtering_flag_from_test(GPU_SAMPLER_FILTERING_MIPMAP, use_filter);
  GPUSamplerExtendMode extend = GPU_SAMPLER_EXTEND_MODE_REPEAT;
  switch (storage->extension) {
    case SHD_IMAGE_EXTENSION_EXTEND:
      extend = GPU_SAMPLER_EXTEND_MODE_EXTEND;
      break;
    case SHD_IMAGE_EXTENSION_MIRROR:
      extend = GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT;
      break;
    case SHD_IMAGE_EXTENSION_CLIP:
      extend = GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER;
      break;
    default:
      break;
  }
  sampler.extend_x = extend;
  sampler.extend_yz = extend;
}

static void material_texture_set(MaterialData &data, const bNode &image_node)
{
  MapTexture map;
  map_texture_set(map, image_node);
  data.texture = map.texture;
  data.sampler = map.sampler;
}

/** Image texture node of an output socket (not tiled), or null. */
static const bNode *image_node_of(const bNodeSocket *from)
{
  if (from == nullptr || from->owner_node().type_legacy != SH_NODE_TEX_IMAGE) {
    return nullptr;
  }
  const bNode &node = from->owner_node();
  const Image *image = reinterpret_cast<const Image *>(node.id);
  return (image && image->source != IMA_SRC_TILED) ? &node : nullptr;
}

/**
 * Scalar input read from an image: its color (red), its alpha, or a channel separated by a
 * Separate Color node (packed maps). False when not linked to an image.
 */
static bool scalar_map_read(const bNode &node, StringRef identifier, MapTexture &r_map)
{
  const bNodeSocket *from = input_link_get(input_find(node, identifier));
  if (from == nullptr) {
    return false;
  }
  int channel = 0;
  if (ELEM(from->owner_node().type_legacy, SH_NODE_SEPARATE_COLOR, SH_NODE_SEPRGB_LEGACY)) {
    const bNode &separate = from->owner_node();
    if (separate.type_legacy == SH_NODE_SEPARATE_COLOR && separate.storage &&
        static_cast<const NodeCombSepColor *>(separate.storage)->mode != NODE_COMBSEP_COLOR_RGB)
    {
      return false;
    }
    const StringRef name = from->identifier;
    channel = (name == "Green" || name == "G") ? 1 : (name == "Blue" || name == "B") ? 2 : 0;
    from = input_link_get(input_find(separate, separate.type_legacy == SH_NODE_SEPARATE_COLOR ?
                                                   "Color" :
                                                   "Image"));
    if (from == nullptr || StringRef(from->identifier) != "Color") {
      return false;
    }
  }
  else if (StringRef(from->identifier) == "Alpha") {
    channel = 3;
  }
  const bNode *image_node = image_node_of(from);
  if (image_node == nullptr) {
    return false;
  }
  map_texture_set(r_map, *image_node);
  r_map.channel = channel;
  return true;
}

/** Tangent space normal map: Normal Map node with an image. */
static void normal_map_read(MaterialData &data, const bNode &node)
{
  const bNodeSocket *from = input_link_get(input_find(node, "Normal"));
  if (from == nullptr || from->owner_node().type_legacy != SH_NODE_NORMAL_MAP) {
    return;
  }
  const bNode &normal_map = from->owner_node();
  const NodeShaderNormalMap *storage = static_cast<const NodeShaderNormalMap *>(
      normal_map.storage);
  if (storage && storage->space != SHD_SPACE_TANGENT) {
    return;
  }
  const bNode *image_node = image_node_of(input_link_get(input_find(normal_map, "Color")));
  if (image_node == nullptr) {
    return;
  }
  map_texture_set(data.normal_map, *image_node);
  data.normal_strength = input_float(normal_map, "Strength", 1.0f);
}

static void base_color_read(MaterialData &data, const bNode &node, StringRef identifier)
{
  if (const bNode *image_node = input_image_node(node, identifier)) {
    data.base_color = float3(1.0f);
    material_texture_set(data, *image_node);
  }
  else {
    data.base_color = input_color(node, identifier, data.base_color);
  }
}

/**
 * Alpha input: its value, or an image linked to it (`coverage_map`, also `texture_alpha` when it
 * is the alpha of the base color's image).
 */
static void alpha_read(MaterialData &data, const bNode &node, StringRef identifier)
{
  data.alpha = input_float(node, identifier, 1.0f);
  MapTexture coverage;
  if (!scalar_map_read(node, identifier, coverage)) {
    return;
  }
  data.coverage_map = coverage;
  data.alpha = 1.0f;
  data.texture_alpha = coverage.channel == 3 && coverage.texture == data.texture;
}

/** Read the BSDF of a shader socket (output socket of a shader node). */
static MaterialData shader_read(const bNodeSocket *shader, int depth)
{
  MaterialData data;
  if (shader == nullptr || depth > 8) {
    return data;
  }
  const bNode &node = shader->owner_node();
  switch (node.type_legacy) {
    case SH_NODE_BSDF_PRINCIPLED: {
      base_color_read(data, node, "Base Color");
      data.metallic = input_float(node, "Metallic", 0.0f);
      data.roughness = input_float(node, "Roughness", 0.5f);
      if (scalar_map_read(node, "Metallic", data.metallic_map)) {
        data.metallic = 1.0f;
      }
      if (scalar_map_read(node, "Roughness", data.roughness_map)) {
        data.roughness = 1.0f;
      }
      normal_map_read(data, node);
      const float ior = input_float(node, "IOR", 1.5f);
      const float level = input_float(node, "Specular IOR Level", 0.5f);
      data.f0 = math::square((ior - 1.0f) / (ior + 1.0f)) * 2.0f * level;
      alpha_read(data, node, "Alpha");
      data.emission = input_color(node, "Emission Color", float3(0.0f)) *
                      input_float(node, "Emission Strength", 0.0f);
      break;
    }
    case SH_NODE_BSDF_DIFFUSE:
      base_color_read(data, node, "Color");
      data.roughness = 1.0f;
      data.f0 = 0.0f;
      break;
    case SH_NODE_BSDF_GLOSSY:
    case SH_NODE_BSDF_METALLIC:
      base_color_read(data, node, node.type_legacy == SH_NODE_BSDF_GLOSSY ? "Color" : "Base Color");
      data.metallic = 1.0f;
      data.roughness = input_float(node, "Roughness", 0.5f);
      break;
    case SH_NODE_BSDF_TRANSPARENT:
      data.base_color = float3(0.0f);
      data.f0 = 0.0f;
      data.alpha = 0.0f;
      break;
    case SH_NODE_EMISSION:
      data.base_color = float3(0.0f);
      data.f0 = 0.0f;
      data.emission = input_color(node, "Color", float3(1.0f)) *
                      input_float(node, "Strength", 1.0f);
      break;
    case SH_NODE_MIX_SHADER:
    case SH_NODE_ADD_SHADER: {
      /* Both shader inputs are named "Shader": use their position. */
      Vector<const bNodeSocket *> shaders;
      for (const bNodeSocket *socket : node.input_sockets()) {
        if (socket->is_available() && socket->type == SOCK_SHADER) {
          shaders.append(input_link_get(socket));
        }
      }
      if (shaders.size() != 2) {
        break;
      }
      /* Transparency mixed with a surface: the factor is its coverage. */
      for (const int k : IndexRange(2)) {
        const bNodeSocket *other = shaders[1 - k];
        if (node.type_legacy != SH_NODE_MIX_SHADER || shaders[k] == nullptr || other == nullptr ||
            shaders[k]->owner_node().type_legacy != SH_NODE_BSDF_TRANSPARENT)
        {
          continue;
        }
        data = shader_read(other, depth + 1);
        if (k == 0) {
          MaterialData coverage = data;
          alpha_read(coverage, node, "Fac");
          data.texture_alpha = coverage.texture_alpha;
          data.coverage_map = coverage.coverage_map;
          data.alpha *= coverage.alpha;
        }
        else {
          data.alpha *= 1.0f - math::clamp(input_float(node, "Fac", 0.5f), 0.0f, 1.0f);
        }
        return data;
      }
      const MaterialData a = shader_read(shaders[0], depth + 1);
      const MaterialData b = shader_read(shaders[1], depth + 1);
      if (node.type_legacy == SH_NODE_ADD_SHADER || shaders[0] == nullptr ||
          shaders[1] == nullptr)
      {
        /* Sum of the emissions, the other parameters of the first BSDF. */
        const MaterialData &main = shaders[0] ? a : b;
        data = main;
        data.emission = (shaders[0] ? a.emission : float3(0.0f)) +
                        (shaders[1] ? b.emission : float3(0.0f));
        break;
      }
      const float fac = math::clamp(input_float(node, "Fac", 0.5f), 0.0f, 1.0f);
      /* Keep the texture of the dominant shader. */
      data = fac < 0.5f ? a : b;
      data.base_color = math::interpolate(a.base_color, b.base_color, fac);
      if (data.texture) {
        data.base_color = (fac < 0.5f ? a : b).base_color;
      }
      data.alpha = math::interpolate(a.alpha, b.alpha, fac);
      data.metallic = math::interpolate(a.metallic, b.metallic, fac);
      data.roughness = math::interpolate(a.roughness, b.roughness, fac);
      data.f0 = math::interpolate(a.f0, b.f0, fac);
      data.emission = math::interpolate(a.emission, b.emission, fac);
      break;
    }
    default:
      /* Other shaders: their color as a rough dielectric. */
      base_color_read(data, node, "Color");
      break;
  }
  return data;
}

MaterialData material_data_get(const Material *material)
{
  MaterialData data;
  if (material == nullptr) {
    return data;
  }
  data.backface_culling = (material->blend_flag & MA_BL_CULL_BACKFACE) != 0;
  const bool blended = material->surface_render_method == MA_SURFACE_METHOD_FORWARD;
  bNodeTree *ntree = material->nodetree;
  const bNode *output = ntree ? ntreeShaderOutputNode(ntree, SHD_OUTPUT_EEVEE) : nullptr;
  if (output == nullptr) {
    data.base_color = float3(material->r, material->g, material->b);
    data.metallic = material->metallic;
    data.roughness = material->roughness;
    data.f0 = 0.08f * material->spec;
    return data;
  }
  ntree->ensure_topology_cache();
  const bool backface_culling = data.backface_culling;
  data = shader_read(input_link_get(input_find(*output, "Surface")), 0);
  data.backface_culling = backface_culling;
  data.blended = blended;
  return data;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lights
 * \{ */

LightData light_data_get(const Object &ob, float light_threshold)
{
  const Light &la = *reinterpret_cast<const Light *>(ob.data);
  LightData light;
  light.object_to_world = ob.object_to_world();
  const float3 power = BKE_light_color(la) * BKE_light_power(la);
  light.diffuse_factor = la.diff_fac;
  light.specular_factor = la.spec_fac;
  light.shadow = (la.mode & LA_SHADOW) != 0;
  /* Conversions of Cycles (`scene/light.cpp`): point and spot lights radiate their power in all
   * directions, area lights from one side (Lambertian), sun strengths are irradiances. */
  switch (la.type) {
    case LA_SUN:
      light.type = LightType::SUN;
      light.intensity = power;
      light.radius = la.sun_angle;
      break;
    case LA_AREA: {
      light.type = LightType::AREA;
      light.intensity = power * float(M_1_PI);
      light.area_round = ELEM(la.area_shape, LA_AREA_DISK, LA_AREA_ELLIPSE);
      const bool square = ELEM(la.area_shape, LA_AREA_SQUARE, LA_AREA_DISK);
      light.area_size = float2(la.area_size, square ? la.area_size : la.area_sizey);
      break;
    }
    case LA_SPOT:
      light.type = LightType::SPOT;
      light.intensity = power * float(0.25 * M_1_PI);
      light.radius = la.radius;
      light.spot_cos_half = math::cos(la.spotsize * 0.5f);
      light.spot_smooth = (1.0f - light.spot_cos_half) * la.spotblend;
      break;
    default:
      light.type = LightType::POINT;
      light.intensity = power * float(0.25 * M_1_PI);
      light.radius = la.radius;
      break;
  }
  if (light.type != LightType::SUN) {
    /* Where a white diffuse surface facing the light receives less than the threshold. */
    const float max_intensity = math::reduce_max(light.intensity);
    light.influence_radius = (la.mode & LA_CUSTOM_ATTENUATION) ?
                                 la.att_dist :
                                 math::sqrt(max_intensity * float(M_1_PI) /
                                            math::max(light_threshold, 1e-6f));
    light.influence_radius = math::max(light.influence_radius, light.radius * 2.0f);
  }
  return light;
}

float3 light_sample_position(const LightData &light, float2 rand)
{
  const float4x4 &m = light.object_to_world;
  const float3 position = m.location();
  switch (light.type) {
    case LightType::SUN: {
      /* Direction toward the sun, within its angle. */
      const float3 z = math::normalize(m.z_axis());
      const float3 x = math::normalize(m.x_axis());
      const float3 y = math::cross(z, x);
      const float cos_max = math::cos(light.radius * 0.5f);
      const float cos_theta = 1.0f - rand.x * (1.0f - cos_max);
      const float sin_theta = math::sqrt(math::max(0.0f, 1.0f - cos_theta * cos_theta));
      const float phi = 2.0f * float(M_PI) * rand.y;
      return z * cos_theta + (x * math::cos(phi) + y * math::sin(phi)) * sin_theta;
    }
    case LightType::AREA: {
      float2 p;
      if (light.area_round) {
        const float r = math::sqrt(rand.x) * 0.5f;
        const float phi = 2.0f * float(M_PI) * rand.y;
        p = float2(math::cos(phi), math::sin(phi)) * r;
      }
      else {
        p = rand - 0.5f;
      }
      p *= light.area_size;
      return position + m.x_axis() * p.x + m.y_axis() * p.y;
    }
    default: {
      if (light.radius <= 0.0f) {
        return position;
      }
      /* Uniform on the sphere: from outside, the same mean light as the center. */
      const float z = 1.0f - 2.0f * rand.x;
      const float r = math::sqrt(math::max(0.0f, 1.0f - z * z));
      const float phi = 2.0f * float(M_PI) * rand.y;
      return position + float3(r * math::cos(phi), r * math::sin(phi), z) * light.radius;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name World
 * \{ */

/**
 * First order spherical harmonics of an equirectangular radiance image (Cycles mapping):
 * sh0 = mean radiance, sh1 = sum(L * dir * solid angle) / (2 pi), so that sh0 + sh1 . n is the
 * irradiance divided by pi.
 */
static WorldLighting world_lighting_from_image(ImBuf *ibuf, float strength, float rotation_z)
{
  WorldLighting world;
  if (ibuf == nullptr || ibuf->x < 2 || ibuf->y < 2) {
    return world;
  }
  /* At most 256 x 128 samples, the low frequencies don't need more. */
  const int step = math::max(1, math::max(ibuf->x / 256, ibuf->y / 128));
  const float cos_r = math::cos(rotation_z), sin_r = math::sin(rotation_z);
  double sum[4][3] = {};
  double total_weight = 0.0;
  for (int y = step / 2; y < ibuf->y; y += step) {
    const float elevation = (float(y) + 0.5f) / float(ibuf->y) * float(M_PI) - float(M_PI_2);
    const float cos_e = math::cos(elevation);
    const float weight = cos_e;
    for (int x = step / 2; x < ibuf->x; x += step) {
      const float phi = ((float(x) + 0.5f) / float(ibuf->x) - 0.5f) * 2.0f * float(M_PI);
      float3 dir(-math::cos(phi) * cos_e, math::sin(phi) * cos_e, math::sin(elevation));
      dir = float3(dir.x * cos_r - dir.y * sin_r, dir.x * sin_r + dir.y * cos_r, dir.z);
      float4 pixel;
      const size_t index = size_t(y) * ibuf->x + x;
      if (ibuf->float_buffer.data) {
        const float *p = ibuf->float_buffer.data + index * ibuf->channels;
        pixel = float4(p[0], ibuf->channels > 1 ? p[1] : p[0], ibuf->channels > 2 ? p[2] : p[0], 1.0f);
      }
      else if (ibuf->byte_buffer.data) {
        const uchar *p = ibuf->byte_buffer.data + index * 4;
        pixel = float4(p[0], p[1], p[2], 255.0f) / 255.0f;
        IMB_colormanagement_colorspace_to_scene_linear_v4(
            pixel, false, ibuf->byte_buffer.colorspace);
      }
      else {
        continue;
      }
      for (int c = 0; c < 3; c++) {
        const double l = double(pixel[c]) * weight;
        sum[0][c] += l;
        sum[1][c] += l * dir.x;
        sum[2][c] += l * dir.y;
        sum[3][c] += l * dir.z;
      }
      total_weight += weight;
    }
  }
  if (total_weight <= 0.0) {
    return world;
  }
  /* total_weight covers 4 pi steradians. */
  const double to_mean = strength / total_weight;
  const double to_sh1 = strength * 4.0 * M_PI / total_weight / (2.0 * M_PI);
  for (int c = 0; c < 3; c++) {
    world.sh0[c] = float(sum[0][c] * to_mean);
    for (int axis = 0; axis < 3; axis++) {
      world.sh1[axis][c] = float(sum[axis + 1][c] * to_sh1);
    }
  }
  world.background = world.sh0;
  return world;
}

static WorldLighting world_lighting_uniform(float3 color)
{
  WorldLighting world;
  world.sh0 = color;
  world.background = color;
  return world;
}

WorldLighting world_lighting_get(const World *world)
{
  if (world == nullptr) {
    return world_lighting_uniform(float3(0.0f));
  }
  const float3 horizon(world->horr, world->horg, world->horb);
  bNodeTree *ntree = world->nodetree;
  const bNode *output = ntree ? ntreeShaderOutputNode(ntree, SHD_OUTPUT_EEVEE) : nullptr;
  if (output == nullptr) {
    return world_lighting_uniform(horizon);
  }
  ntree->ensure_topology_cache();
  const bNodeSocket *from = input_link_get(input_find(*output, "Surface"));
  if (from == nullptr || from->owner_node().type_legacy != SH_NODE_BACKGROUND) {
    return world_lighting_uniform(float3(0.0f));
  }
  const bNode &background = from->owner_node();
  const float strength = input_float(background, "Strength", 1.0f);
  const bNodeSocket *color_link = input_link_get(input_find(background, "Color"));
  if (color_link && color_link->owner_node().type_legacy == SH_NODE_TEX_ENVIRONMENT) {
    const bNode &env = color_link->owner_node();
    Image *image = reinterpret_cast<Image *>(env.id);
    if (image) {
      const NodeTexEnvironment *storage = static_cast<const NodeTexEnvironment *>(env.storage);
      ImageUser iuser = storage->iuser;
      void *lock;
      ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
      WorldLighting result = world_lighting_from_image(ibuf, strength, 0.0f);
      BKE_image_release_ibuf(image, ibuf, lock);
      return result;
    }
  }
  return world_lighting_uniform(input_color(background, "Color", horizon) * strength);
}

WorldLighting world_lighting_studio_get(const View3D &v3d)
{
  StudioLight *sl = BKE_studiolight_find(v3d.shading.lookdev_light, STUDIOLIGHT_TYPE_WORLD);
  if (sl == nullptr) {
    return world_lighting_uniform(float3(0.05f));
  }
  BKE_studiolight_ensure_flag(sl, STUDIOLIGHT_EXTERNAL_IMAGE_LOADED);
  /* The image is the same for every redraw: compute it once. */
  static const StudioLight *cached_light = nullptr;
  static float cached_rotation = 0.0f;
  static WorldLighting cached;
  if (cached_light != sl || cached_rotation != v3d.shading.studiolight_rot_z) {
    cached = world_lighting_from_image(
        sl->equirect_radiance_buffer, 1.0f, v3d.shading.studiolight_rot_z);
    cached_light = sl;
    cached_rotation = v3d.shading.studiolight_rot_z;
  }
  WorldLighting world = cached;
  const float intensity = v3d.shading.studiolight_intensity;
  world.sh0 *= intensity;
  for (float3 &sh : world.sh1) {
    sh *= intensity;
  }
  world.background = world.sh0;
  return world;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampling
 * \{ */

float halton(int index, int base)
{
  float result = 0.0f;
  float f = 1.0f / float(base);
  while (index > 0) {
    result += f * float(index % base);
    index /= base;
    f /= float(base);
  }
  return result;
}

/** \} */

}  // namespace blender::mechazyiu
