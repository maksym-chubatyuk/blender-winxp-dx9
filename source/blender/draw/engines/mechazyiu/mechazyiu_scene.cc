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
 * Direction of a point of an equirectangular image (Cycles mapping), u and v in [0, 1], v = 0 at
 * the bottom.
 */
static float3 equirect_direction(float u, float v)
{
  const float phi = (0.5f - u) * 2.0f * float(M_PI);
  const float elevation = (v - 0.5f) * float(M_PI);
  return float3(math::cos(phi) * math::cos(elevation),
                math::sin(phi) * math::cos(elevation),
                math::sin(elevation));
}

/** Scene linear colors of an equirectangular image, row 0 at the bottom. */
struct LinearImage {
  int width = 0;
  int height = 0;
  Array<float3> pixels;

  float3 &at(int x, int y)
  {
    return pixels[size_t(y) * width + x];
  }
  const float3 &at(int x, int y) const
  {
    return pixels[size_t(y) * width + x];
  }

  /** Bilinear, the horizontal direction wraps around. */
  float3 sample(const float3 &dir) const
  {
    const float u = 0.5f - math::atan2(dir.y, dir.x) / (2.0f * float(M_PI));
    const float v = 0.5f + math::asin(math::clamp(dir.z, -1.0f, 1.0f)) / float(M_PI);
    const float x = u * width - 0.5f, y = math::clamp(v * height - 0.5f, 0.0f, height - 1.0f);
    const int x0 = int(math::floor(x)), y0 = int(y);
    const float fx = x - float(x0), fy = y - float(y0);
    const int y1 = math::min(y0 + 1, height - 1);
    auto wrap = [&](int px) { return ((px % width) + width) % width; };
    return math::interpolate(math::interpolate(at(wrap(x0), y0), at(wrap(x0 + 1), y0), fx),
                             math::interpolate(at(wrap(x0), y1), at(wrap(x0 + 1), y1), fx),
                             fy);
  }

  /** Average of the pixels covering each pixel of a `w` x `h` image (no larger than this). */
  LinearImage downsample(int w, int h) const
  {
    LinearImage result;
    result.width = w;
    result.height = h;
    result.pixels = Array<float3>(size_t(w) * h, float3(0.0f));
    Array<int> counts(size_t(w) * h, 0);
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        const size_t i = size_t(y * h / height) * w + size_t(x * w / width);
        result.pixels[i] += at(x, y);
        counts[i]++;
      }
    }
    for (const int64_t i : result.pixels.index_range()) {
      result.pixels[i] /= float(math::max(counts[i], 1));
    }
    return result;
  }

  /**
   * Gaussian blur of `sigma` radians, separable: the horizontal kernel widens toward the poles
   * (a horizontal pixel covers less angle there).
   */
  void blur(float sigma)
  {
    const float pixel_angle = float(M_PI) / height;
    Array<float3> line(math::max(width, height));
    Vector<float> kernel;
    auto kernel_build = [&](float sigma_px, int max_radius) {
      const int radius = math::min(int(math::ceil(sigma_px * 3.0f)), max_radius);
      kernel.resize(radius + 1);
      for (int i = 0; i <= radius; i++) {
        kernel[i] = math::exp(-0.5f * float(i * i) / (sigma_px * sigma_px));
      }
      return radius;
    };
    /* Vertical, clamped at the poles. */
    const int radius_v = kernel_build(math::max(sigma / pixel_angle, 0.5f), height - 1);
    for (int x = 0; x < width; x++) {
      for (int y = 0; y < height; y++) {
        float3 sum(0.0f);
        float total = 0.0f;
        for (int i = -radius_v; i <= radius_v; i++) {
          const int py = y + i;
          if (py >= 0 && py < height) {
            sum += at(x, py) * kernel[math::abs(i)];
            total += kernel[math::abs(i)];
          }
        }
        line[y] = sum / total;
      }
      for (int y = 0; y < height; y++) {
        at(x, y) = line[y];
      }
    }
    /* Horizontal, wrapping around. */
    for (int y = 0; y < height; y++) {
      const float elevation = (float(y) + 0.5f) / height * float(M_PI) - float(M_PI_2);
      const float sigma_px = math::max(
          sigma / (2.0f * float(M_PI) / width * math::max(math::cos(elevation), 0.01f)), 0.5f);
      const int radius = kernel_build(sigma_px, width / 2);
      for (int x = 0; x < width; x++) {
        float3 sum(0.0f);
        float total = 0.0f;
        for (int i = -radius; i <= radius; i++) {
          sum += at(((x + i) % width + width) % width, y) * kernel[math::abs(i)];
          total += kernel[math::abs(i)];
        }
        line[x] = sum / total;
      }
      for (int x = 0; x < width; x++) {
        at(x, y) = line[x];
      }
    }
  }
};

static LinearImage linear_image_get(ImBuf *ibuf, int max_width)
{
  LinearImage image;
  if (ibuf == nullptr || ibuf->x < 2 || ibuf->y < 2 ||
      !(ibuf->float_buffer.data || ibuf->byte_buffer.data))
  {
    return image;
  }
  const int step = math::max(1, (ibuf->x + max_width - 1) / max_width);
  image.width = ibuf->x / step;
  image.height = math::max(1, ibuf->y / step);
  image.pixels.reinitialize(size_t(image.width) * image.height);
  for (int y = 0; y < image.height; y++) {
    for (int x = 0; x < image.width; x++) {
      const size_t index = size_t(y * step) * ibuf->x + size_t(x * step);
      float4 pixel;
      if (ibuf->float_buffer.data) {
        const float *p = ibuf->float_buffer.data + index * ibuf->channels;
        pixel = float4(p[0],
                       ibuf->channels > 1 ? p[1] : p[0],
                       ibuf->channels > 2 ? p[2] : p[0],
                       1.0f);
      }
      else {
        const uchar *p = ibuf->byte_buffer.data + index * 4;
        pixel = float4(p[0], p[1], p[2], 255.0f) / 255.0f;
        IMB_colormanagement_colorspace_to_scene_linear_v4(
            pixel, false, ibuf->byte_buffer.colorspace);
      }
      image.at(x, y) = float3(pixel);
    }
  }
  return image;
}

/**
 * First order spherical harmonics of an equirectangular radiance image:
 * sh0 = mean radiance, sh1 = sum(L * dir * solid angle) / (2 pi), so that sh0 + sh1 . n is the
 * irradiance divided by pi. `image` is small (the low frequencies don't need more).
 */
static WorldLighting world_lighting_from_image(const LinearImage &image)
{
  WorldLighting world;
  double sum[4][3] = {};
  double total_weight = 0.0;
  for (int y = 0; y < image.height; y++) {
    const float v = (float(y) + 0.5f) / image.height;
    const float weight = math::cos((v - 0.5f) * float(M_PI));
    for (int x = 0; x < image.width; x++) {
      const float3 dir = equirect_direction((float(x) + 0.5f) / image.width, v);
      const float3 &pixel = image.at(x, y);
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
  const double to_mean = 1.0 / total_weight;
  const double to_sh1 = 4.0 * M_PI / total_weight / (2.0 * M_PI);
  for (int c = 0; c < 3; c++) {
    world.sh0[c] = float(sum[0][c] * to_mean);
    for (int axis = 0; axis < 3; axis++) {
      world.sh1[axis][c] = float(sum[axis + 1][c] * to_sh1);
    }
  }
  world.background = world.sh0;
  return world;
}

/** Direction of a texel of a cube face, `s` and `t` in [-1, 1] (Direct3D cube map layout). */
static float3 cube_direction(int face, float s, float t)
{
  switch (face) {
    case 0:
      return float3(1.0f, -t, -s);
    case 1:
      return float3(-1.0f, -t, s);
    case 2:
      return float3(s, 1.0f, t);
    case 3:
      return float3(s, -1.0f, -t);
    case 4:
      return float3(s, -t, 1.0f);
    default:
      return float3(-s, -t, -1.0f);
  }
}

/** Iterates the texels of the 6 faces of a cube level, with their direction (normalized). */
template<typename Fn> static void cube_texels(int size, Fn fn)
{
  for (int face = 0; face < 6; face++) {
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        const float s = (float(x) + 0.5f) / size * 2.0f - 1.0f;
        const float t = (float(y) + 0.5f) / size * 2.0f - 1.0f;
        fn((size_t(face) * size + y) * size + x, math::normalize(cube_direction(face, s, t)));
      }
    }
  }
}

static void rgbm_encode(const float3 &color, uchar *r_rgba)
{
  const float m = math::clamp(math::reduce_max(color) / environment_rgbm_range, 1e-6f, 1.0f);
  const float a = math::ceil(m * 255.0f) / 255.0f;
  const float3 rgb = math::clamp(color / (a * environment_rgbm_range), 0.0f, 1.0f);
  r_rgba[0] = uchar(rgb.x * 255.0f + 0.5f);
  r_rgba[1] = uchar(rgb.y * 255.0f + 0.5f);
  r_rgba[2] = uchar(rgb.z * 255.0f + 0.5f);
  r_rgba[3] = uchar(a * 255.0f + 0.5f);
}

static Array<uchar> rgbm_encode(const Span<float3> colors)
{
  Array<uchar> bytes(colors.size() * 4);
  for (const int64_t i : colors.index_range()) {
    rgbm_encode(colors[i], &bytes[i * 4]);
  }
  return bytes;
}

/** A cube of `size` texels per face from an image, mip levels by averaging down to `min_size`. */
static EnvironmentCube environment_cube_build(const LinearImage &image, int size, int min_size)
{
  EnvironmentCube cube;
  cube.size = size;
  Array<float3> level(size_t(6) * size * size);
  cube_texels(size, [&](size_t index, const float3 &d) { level[index] = image.sample(d); });
  for (int level_size = size;; level_size /= 2) {
    cube.levels.append(rgbm_encode(level));
    if (level_size <= min_size) {
      break;
    }
    const int half = level_size / 2;
    Array<float3> next(size_t(6) * half * half);
    for (int face = 0; face < 6; face++) {
      for (int y = 0; y < half; y++) {
        for (int x = 0; x < half; x++) {
          auto at = [&](int px, int py) {
            return level[(size_t(face) * level_size + py) * level_size + px];
          };
          next[(size_t(face) * half + y) * half + x] = (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) +
                                                        at(2 * x, 2 * y + 1) +
                                                        at(2 * x + 1, 2 * y + 1)) *
                                                       0.25f;
        }
      }
    }
    level = std::move(next);
  }
  return cube;
}

/** Cube of the cosine weighted integral of the radiance divided by pi (irradiance / pi). */
static EnvironmentCube irradiance_cube_build(const LinearImage &image, int size)
{
  const LinearImage small = image.downsample(64, 32);
  Array<float3> directions(small.pixels.size());
  Array<float> weights(small.pixels.size());
  for (int y = 0; y < small.height; y++) {
    const float v = (float(y) + 0.5f) / small.height;
    for (int x = 0; x < small.width; x++) {
      directions[y * small.width + x] = equirect_direction((float(x) + 0.5f) / small.width, v);
      /* Solid angle / pi. */
      weights[y * small.width + x] = (2.0f * float(M_PI) / small.width) *
                                     (float(M_PI) / small.height) *
                                     math::cos((v - 0.5f) * float(M_PI)) / float(M_PI);
    }
  }
  Array<float3> texels(size_t(6) * size * size);
  cube_texels(size, [&](size_t index, const float3 &n) {
    float3 sum(0.0f);
    for (const int64_t i : small.pixels.index_range()) {
      const float cos_theta = math::dot(n, directions[i]);
      if (cos_theta > 0.0f) {
        sum += small.pixels[i] * (cos_theta * weights[i]);
      }
    }
    texels[index] = sum;
  });
  EnvironmentCube cube;
  cube.size = size;
  cube.levels.append(rgbm_encode(texels));
  return cube;
}

/** Width of the blur of the reflections of a roughness: about the GGX lobe's (radians). */
static float roughness_blur(float roughness)
{
  return 1.1f * roughness * roughness;
}

static std::shared_ptr<EnvironmentMaps> environment_maps_build(const LinearImage &image)
{
  std::shared_ptr<EnvironmentMaps> maps = std::make_shared<EnvironmentMaps>();
  /* Sharp: as sharp as the image, mip levels for distant or curved surfaces. */
  maps->cubes[0] = environment_cube_build(image, image.width >= 1024 ? 512 : 256, 16);
  /* Blurred: texels about half the blur wide. */
  LinearImage glossy = image.downsample(256, 128);
  glossy.blur(roughness_blur(1.0f / 3.0f));
  maps->cubes[1] = environment_cube_build(glossy, 32, 8);
  LinearImage rough = image.downsample(64, 32);
  rough.blur(roughness_blur(2.0f / 3.0f));
  /* Large enough that the edges of the faces (not filtered across) don't show. */
  maps->cubes[2] = environment_cube_build(rough, 16, 16);
  maps->cubes[3] = irradiance_cube_build(image, 32);
  return maps;
}

std::shared_ptr<EnvironmentMaps> environment_maps_uniform(const float3 &color)
{
  std::shared_ptr<EnvironmentMaps> maps = std::make_shared<EnvironmentMaps>();
  for (EnvironmentCube &cube : maps->cubes) {
    cube.size = 1;
    Array<uchar> bytes(6 * 4);
    for (int face = 0; face < 6; face++) {
      rgbm_encode(color, &bytes[face * 4]);
    }
    cube.levels.append(std::move(bytes));
  }
  return maps;
}

/**
 * Lighting and cubes of an environment image, kept while the image buffer is the same (they take
 * long on old CPUs).
 */
static WorldLighting environment_lighting_get(ImBuf *ibuf, const void *source)
{
  static const void *cached_source = nullptr;
  static const ImBuf *cached_ibuf = nullptr;
  static int cached_size[2] = {0, 0};
  static WorldLighting cached;
  if (source != cached_source || ibuf != cached_ibuf || ibuf == nullptr ||
      ibuf->x != cached_size[0] || ibuf->y != cached_size[1])
  {
    /* 2048 wide: the largest texture of the R300, and as sharp as its cube faces. */
    const LinearImage image = linear_image_get(ibuf, 2048);
    cached = WorldLighting();
    if (image.width > 0) {
      cached = world_lighting_from_image(image.downsample(256, 128));
      cached.maps = environment_maps_build(image);
    }
    cached_source = source;
    cached_ibuf = ibuf;
    cached_size[0] = ibuf ? ibuf->x : 0;
    cached_size[1] = ibuf ? ibuf->y : 0;
  }
  return cached;
}

static void world_scale(WorldLighting &world, float strength)
{
  world.sh0 *= strength;
  for (float3 &sh : world.sh1) {
    sh *= strength;
  }
  world.background *= strength;
  world.strength *= strength;
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
      WorldLighting result = environment_lighting_get(ibuf, image);
      BKE_image_release_ibuf(image, ibuf, lock);
      world_scale(result, strength);
      /* A Mapping node turning the image around Z (it rotates the lookup direction, the
       * environment turns the other way). */
      const bNodeSocket *vector_link = input_link_get(input_find(env, "Vector"));
      if (vector_link && vector_link->owner_node().type_legacy == SH_NODE_MAPPING) {
        const bNodeSocket *rotation = input_find(vector_link->owner_node(), "Rotation");
        if (rotation && rotation->type == SOCK_VECTOR && !input_link_get(rotation)) {
          result.rotation_z = -rotation->default_value_typed<bNodeSocketValueVector>()->value[2];
        }
      }
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
  WorldLighting world = environment_lighting_get(sl->equirect_radiance_buffer, sl);
  /* Like EEVEE's look-dev world: the lookups turn by the angle. */
  world.rotation_z = -v3d.shading.studiolight_rot_z;
  world.view_space = (v3d.shading.flag & V3D_SHADING_STUDIOLIGHT_VIEW_ROTATION) != 0;
  world_scale(world, v3d.shading.studiolight_intensity);
  /* Like EEVEE's material preview: the studio light blurred, partly visible. */
  world.background_opacity = v3d.shading.studiolight_background;
  world.background_blur = v3d.shading.studiolight_blur;
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
