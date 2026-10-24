/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "libocio_gpu_shader_binder.hh"

#include <cmath>

#include "BLI_array.hh"

#include "GPU_context.hh"
#include "GPU_texture.hh"

#include "error_handling.hh"
#include "libocio_config.hh"
#include "libocio_display_processor.hh"
#include "libocio_processor.hh"

#include "../gpu_shader_binder_internal.hh"

namespace blender::ocio {

namespace {

using namespace OCIO_NAMESPACE;

static ConstProcessorRcPtr create_to_scene_linear_processor(
    const ConstConfigRcPtr &ocio_config, const internal::GPUDisplayShader &display_shader)
{
  return create_ocio_processor(
      ocio_config, display_shader.from_colorspace.c_str(), ROLE_SCENE_LINEAR);
}

static ConstProcessorRcPtr create_to_display_processor(
    const LibOCIOConfig &config, const internal::GPUDisplayShader &display_shader)
{
  DisplayParameters display_parameters;
  display_parameters.from_colorspace = ROLE_SCENE_LINEAR;
  display_parameters.view = display_shader.view;
  display_parameters.display = display_shader.display;
  display_parameters.look = display_shader.look;
  display_parameters.use_hdr_buffer = display_shader.use_hdr_buffer;
  display_parameters.use_display_emulation = display_shader.use_display_emulation;
  display_parameters.use_scope_space = display_shader.use_scope_space;
  return create_ocio_display_processor(config, display_parameters);
}

static bool add_gpu_uniform(internal::GPUTextures &textures,
                            const GpuShaderDescRcPtr &shader_desc,
                            const int index)
{
  internal::GPUUniform uniform;
  uniform.name = shader_desc->getUniform(index, uniform.data);
  if (uniform.data.m_type == UNIFORM_UNKNOWN) {
    return false;
  }

  textures.uniforms.append(uniform);
  return true;
}

static bool add_gpu_lut_1D2D(internal::GPUTextures &textures,
                             const GpuShaderDescRcPtr &shader_desc,
                             const int index)
{
  const char *texture_name = nullptr;
  const char *sampler_name = nullptr;
  uint width = 0;
  uint height = 0;

  GpuShaderCreator::TextureType channel = GpuShaderCreator::TEXTURE_RGB_CHANNEL;
  Interpolation interpolation = INTERP_LINEAR;

  /* Always use 2D textures in OpenColorIO 2.3, simpler and same performance. */
  static_assert(OCIO_VERSION_HEX >= 0x02030000);
  GpuShaderDesc::TextureDimensions dimensions = GpuShaderDesc::TEXTURE_2D;
  shader_desc->getTexture(
      index, texture_name, sampler_name, width, height, channel, dimensions, interpolation);

  const float *values;
  shader_desc->getTextureValues(index, values);
  if (texture_name == nullptr || sampler_name == nullptr || width == 0 || height == 0 ||
      values == nullptr)
  {
    return false;
  }

  gpu::TextureFormat format = (channel == GpuShaderCreator::TEXTURE_RGB_CHANNEL) ?
                                  gpu::TextureFormat::SFLOAT_16_16_16 :
                                  gpu::TextureFormat::SFLOAT_16;

  internal::GPULutTexture lut;
  /* There does not appear to be an explicit way to check if a texture is 1D or 2D.
   * It depends on more than height. So check instead by looking at the source.
   * The Blender default config does not use 1D textures, but for example
   * studio-config-v3.0.0_aces-v2.0_ocio-v2.4.ocio needs this code. */
  std::string sampler1D_name = std::string("sampler1D ") + sampler_name;
  if (strstr(shader_desc->getShaderText(), sampler1D_name.c_str()) != nullptr) {
    lut.texture = GPU_texture_create_1d(
        texture_name, width, 1, format, GPU_TEXTURE_USAGE_SHADER_READ, values);
  }
  else {
    lut.texture = GPU_texture_create_2d(
        texture_name, width, height, 1, format, GPU_TEXTURE_USAGE_SHADER_READ, values);
  }
  if (lut.texture == nullptr) {
    return false;
  }

  GPU_texture_filter_mode(lut.texture, interpolation != INTERP_NEAREST);
  GPU_texture_extend_mode(lut.texture, GPU_SAMPLER_EXTEND_MODE_EXTEND);

  lut.sampler_name = sampler_name;

  textures.luts.append(lut);

  return true;
}

static bool add_gpu_lut_3D(internal::GPUTextures &textures,
                           const GpuShaderDescRcPtr &shader_desc,
                           const int index)
{
  const char *texture_name = nullptr;
  const char *sampler_name = nullptr;
  uint edgelen = 0;
  Interpolation interpolation = INTERP_LINEAR;
  shader_desc->get3DTexture(index, texture_name, sampler_name, edgelen, interpolation);

  const float *values;
  shader_desc->get3DTextureValues(index, values);
  if (texture_name == nullptr || sampler_name == nullptr || edgelen == 0 || values == nullptr) {
    return false;
  }

  internal::GPULutTexture lut;
  lut.texture = GPU_texture_create_3d(texture_name,
                                      edgelen,
                                      edgelen,
                                      edgelen,
                                      1,
                                      gpu::TextureFormat::SFLOAT_16_16_16,
                                      GPU_TEXTURE_USAGE_SHADER_READ,
                                      values);
  if (lut.texture == nullptr) {
    return false;
  }

  GPU_texture_filter_mode(lut.texture, interpolation != INTERP_NEAREST);
  GPU_texture_extend_mode(lut.texture, GPU_SAMPLER_EXTEND_MODE_EXTEND);

  lut.sampler_name = sampler_name;

  textures.luts.append(lut);
  return true;
}

static bool create_gpu_textures(internal::GPUTextures &textures,
                                const GpuShaderDescRcPtr &shader_desc)
{
  for (int index = 0; index < shader_desc->getNumUniforms(); index++) {
    if (!add_gpu_uniform(textures, shader_desc, index)) {
      return false;
    }
  }
  for (int index = 0; index < shader_desc->getNumTextures(); index++) {
    if (!add_gpu_lut_1D2D(textures, shader_desc, index)) {
      return false;
    }
  }
  for (int index = 0; index < shader_desc->getNum3DTextures(); index++) {
    if (!add_gpu_lut_3D(textures, shader_desc, index)) {
      return false;
    }
  }

  return true;
}

/* View transform lattice of Direct3D 9 (see `ocio_display.hlsl`): over log2 of the scene linear
 * color, the range of the AgX log encoding (25 stops). */
constexpr int d3d9_lut_size = 32;
constexpr float d3d9_lut_log2_min = -12.47393f;
constexpr float d3d9_lut_log2_range = 25.0f;

/**
 * The view transform of Direct3D 9 hardware (shader model 2: no float volume textures on some, no
 * room for OpenColorIO's shaders): the display processor sampled on the CPU into a lattice of
 * 8 bit colors, the square root of the display linear color (decoded from sRGB). Standard sRGB
 * views keep the exact transform of the shader.
 */
static bool d3d9_view_lut_create(internal::GPUTextures &textures,
                                 const ConstProcessorRcPtr &processor_to_display)
{
  const int n = d3d9_lut_size;
  Array<float> colors(size_t(n) * n * n * 3);
  float *color = colors.data();
  for (int b = 0; b < n; b++) {
    for (int g = 0; g < n; g++) {
      for (int r = 0; r < n; r++) {
        for (const int i : {r, g, b}) {
          /* The first point is black. */
          *color++ = (i == 0) ? 0.0f :
                                std::exp2(d3d9_lut_log2_min +
                                          d3d9_lut_log2_range * float(i) / float(n - 1));
        }
      }
    }
  }
  try {
    PackedImageDesc image(colors.data(), n * n * n, 1, 3);
    processor_to_display->getDefaultCPUProcessor()->apply(image);
  }
  catch (Exception &exception) {
    report_exception(exception);
    return false;
  }
  Array<uchar> texels(size_t(n) * n * n * 4);
  for (const int64_t i : IndexRange(int64_t(n) * n * n)) {
    for (int c = 0; c < 3; c++) {
      const float encoded = std::clamp(colors[i * 3 + c], 0.0f, 1.0f);
      const float linear = (encoded < 0.04045f) ? encoded / 12.92f :
                                                  std::pow((encoded + 0.055f) / 1.055f, 2.4f);
      texels[i * 4 + c] = uchar(std::sqrt(linear) * 255.0f + 0.5f);
    }
    texels[i * 4 + 3] = 255;
  }

  internal::GPULutTexture lut;
  lut.texture = GPU_texture_create_3d("OCIOViewLUT",
                                      n,
                                      n,
                                      n,
                                      1,
                                      gpu::TextureFormat::UNORM_8_8_8_8,
                                      GPU_TEXTURE_USAGE_SHADER_READ,
                                      nullptr);
  if (lut.texture == nullptr) {
    return false;
  }
  GPU_texture_update(lut.texture, GPU_DATA_UBYTE, texels.data());
  GPU_texture_filter_mode(lut.texture, true);
  GPU_texture_extend_mode(lut.texture, GPU_SAMPLER_EXTEND_MODE_EXTEND);
  lut.sampler_name = "view_lut";
  textures.luts.append(lut);
  return true;
}

}  // namespace

void LibOCIOGPUShaderBinder::construct_shader_for_processors(
    internal::GPUDisplayShader &display_shader,
    const ConstProcessorRcPtr &processor_to_scene_linear,
    const ConstProcessorRcPtr &processor_to_display,
    const Span<std::array<StringRefNull, 2>> additional_defines) const
{
  std::string fragment_source;

  GpuShaderDescRcPtr shaderdesc_to_scene_linear;
  if (processor_to_scene_linear) {
    shaderdesc_to_scene_linear = GpuShaderDesc::CreateShaderDesc();
    shaderdesc_to_scene_linear->setLanguage(GPU_LANGUAGE_GLSL_1_3);
    shaderdesc_to_scene_linear->setFunctionName("OCIO_to_scene_linear");
    shaderdesc_to_scene_linear->setResourcePrefix("to_scene");
    processor_to_scene_linear->getDefaultGPUProcessor()->extractGpuShaderInfo(
        shaderdesc_to_scene_linear);
    shaderdesc_to_scene_linear->finalize();

    if (!create_gpu_textures(display_shader.textures, shaderdesc_to_scene_linear)) {
      display_shader.is_valid = false;
      return;
    }

    fragment_source += shaderdesc_to_scene_linear->getShaderText();
    fragment_source += "\n";
  }

  GpuShaderDescRcPtr shaderdesc_to_display;
  if (processor_to_display) {
    shaderdesc_to_display = GpuShaderDesc::CreateShaderDesc();
    shaderdesc_to_display->setLanguage(GPU_LANGUAGE_GLSL_1_3);
    shaderdesc_to_display->setFunctionName("OCIO_to_display");
    shaderdesc_to_display->setResourcePrefix("to_display");
    processor_to_display->getDefaultGPUProcessor()->extractGpuShaderInfo(shaderdesc_to_display);
    shaderdesc_to_display->finalize();

    if (!create_gpu_textures(display_shader.textures, shaderdesc_to_display)) {
      display_shader.is_valid = false;
      return;
    }

    fragment_source += shaderdesc_to_display->getShaderText();
    fragment_source += "\n";
  }

  if (!create_gpu_shader(display_shader, fragment_source, additional_defines)) {
    display_shader.is_valid = false;
    return;
  }

  display_shader.is_valid = true;
}

static const std::array<StringRefNull, 2> d3d9_lut_define[1] = {{"D3D9_VIEW_LUT", ""}};

void LibOCIOGPUShaderBinder::construct_display_shader(
    internal::GPUDisplayShader &display_shader) const
{
  const LibOCIOConfig &config = static_cast<const LibOCIOConfig &>(config_);
  const OCIO_NAMESPACE::ConstConfigRcPtr &ocio_config = config.get_ocio_config();

  ConstProcessorRcPtr processor_to_scene_linear = create_to_scene_linear_processor(ocio_config,
                                                                                   display_shader);
  ConstProcessorRcPtr processor_to_display = create_to_display_processor(config, display_shader);

  if (!processor_to_scene_linear || !processor_to_display) {
    display_shader.is_valid = false;
    return;
  }

  if (GPU_backend_get_type() == GPU_BACKEND_D3D9) {
    /* The shader of Direct3D 9 converts from scene linear; Standard sRGB exactly, other views
     * through a lattice. */
    const bool standard = display_shader.view == "Standard" && display_shader.look.empty() &&
                          display_shader.display == "sRGB";
    if (!standard && !d3d9_view_lut_create(display_shader.textures, processor_to_display)) {
      display_shader.is_valid = false;
      return;
    }
    display_shader.is_valid = create_gpu_shader(
        display_shader,
        "",
        standard ? Span<std::array<StringRefNull, 2>>() :
                   Span<std::array<StringRefNull, 2>>(d3d9_lut_define, 1));
    return;
  }

  construct_shader_for_processors(
      display_shader, processor_to_scene_linear, processor_to_display, {});
}

void LibOCIOGPUShaderBinder::construct_scene_linear_shader(
    internal::GPUDisplayShader &display_shader) const
{
  const LibOCIOConfig &config = static_cast<const LibOCIOConfig &>(config_);
  const OCIO_NAMESPACE::ConstConfigRcPtr &ocio_config = config.get_ocio_config();

  ConstProcessorRcPtr processor_to_scene_linear = create_to_scene_linear_processor(ocio_config,
                                                                                   display_shader);
  if (!processor_to_scene_linear) {
    display_shader.is_valid = false;
    return;
  }

  construct_shader_for_processors(
      display_shader,
      processor_to_scene_linear,
      nullptr,
      {{"USE_TO_SCENE_LINEAR_ONLY", ""}, {"OUTPUT_PREMULTIPLIED", ""}});
}

}  // namespace blender::ocio
