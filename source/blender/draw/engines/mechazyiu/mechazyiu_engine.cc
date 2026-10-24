/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * MECHAZYIU render engine: see `mechazyiu_private.hh`.
 *
 * A sample of a tile:
 * 1. The surfaces fill the G-buffer (4 render targets).
 * 2. The base pass mixes the world light, emission and background into the running mean.
 * 3. Each light adds its share over the rectangle it reaches: R300 hardware can't blend into
 *    floating point targets, so the light pass reads the mean and writes the other buffer, and
 *    the rectangle is copied back.
 * The viewport keeps the mean of the whole view (`mean_tx_`) between redraws and adds one sample
 * per redraw. Final renders do every sample of a tile before the next tile.
 */

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_report.hh"
#include "BKE_editmesh.hh"
#include "BKE_idprop.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph_query.hh"

#include "BKE_camera.h"

#include "DNA_camera_types.h"
#include "DNA_light_types.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "GPU_capabilities.hh"
#include "GPU_context.hh"
#include "GPU_framebuffer.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

#include "RE_engine.h"
#include "RE_pipeline.h"

#include "bmesh.hh"

#include "DRW_engine.hh"

#include "draw_cache.hh"
#include "draw_manager.hh"
#include "draw_pass.hh"
#include "draw_view.hh"
#include "draw_view_data.hh"

#include "mechazyiu_engine.h" /* Own include. */
#include "mechazyiu_private.hh"

namespace blender {

namespace mechazyiu {

using namespace draw;

/** Materials of a view (the index is in the G-buffer, 8 bits). */
constexpr int material_table_len = 256;

/** Video memory per pixel of a tile: 4 G-buffer targets, depth, the sample and its copy (half
 * floats), the shadow mask, the mean of the samples (final renders). */
constexpr int64_t tile_bytes_per_pixel = 4 * 4 + 4 + 2 * 8 + 4 + 8;
/** Shadow map atlas: 4 x 2 cells of 512 (the faces of point and area lights), or the left half
 * (spots and suns). Distances in 24 bits of 8 bit colors, and their depth buffer. */
constexpr int shadow_atlas_width = 2048;
constexpr int shadow_atlas_height = 1024;
constexpr int shadow_cube_size = 512;
constexpr int64_t shadow_atlas_bytes = int64_t(shadow_atlas_width) * shadow_atlas_height * 8 +
                                       int64_t(shadow_atlas_width) * shadow_atlas_height * 12 / 4;
/** Running mean (half float) and linear depth of the whole viewport. */
constexpr int64_t view_bytes_per_pixel = 8 + 4;

/* -------------------------------------------------------------------- */
/** \name Shaders
 * \{ */

class ShaderCache {
 public:
  gpu::StaticShader gbuffer[3 /*SurfaceKind*/][2 /*flip*/][2 /*single target*/] = {
      {{{"mechazyiu_gbuffer"}, {"mechazyiu_gbuffer_single"}},
       {{"mechazyiu_gbuffer_flip"}, {"mechazyiu_gbuffer_flip_single"}}},
      {{{"mechazyiu_gbuffer_texture"}, {"mechazyiu_gbuffer_texture_single"}},
       {{"mechazyiu_gbuffer_texture_flip"}, {"mechazyiu_gbuffer_texture_flip_single"}}},
      {{{"mechazyiu_gbuffer_maps"}, {"mechazyiu_gbuffer_maps_single"}},
       {{"mechazyiu_gbuffer_maps_flip"}, {"mechazyiu_gbuffer_maps_flip_single"}}}};
  gpu::StaticShader base = {"mechazyiu_base"};
  gpu::StaticShader ao[2 /*ortho*/] = {{"mechazyiu_ao"}, {"mechazyiu_ao_ortho"}};
  gpu::StaticShader bounce = {"mechazyiu_bounce"};
  gpu::StaticShader light[2 /*blend*/][light_type_len] = {
      {{"mechazyiu_light_point"},
       {"mechazyiu_light_spot"},
       {"mechazyiu_light_sun"},
       {"mechazyiu_light_area"}},
      {{"mechazyiu_light_point_blend"},
       {"mechazyiu_light_spot_blend"},
       {"mechazyiu_light_sun_blend"},
       {"mechazyiu_light_area_blend"}}};
  gpu::StaticShader mix = {"mechazyiu_mix"};
  gpu::StaticShader shadow[2 /*ortho*/] = {{"mechazyiu_shadow"}, {"mechazyiu_shadow_ortho"}};
  gpu::StaticShader shadow_cutout[2 /*ortho*/] = {{"mechazyiu_shadow_cutout"},
                                                  {"mechazyiu_shadow_ortho_cutout"}};
  gpu::StaticShader shadow_transmit[2 /*texture*/] = {{"mechazyiu_shadow_transmit"},
                                                      {"mechazyiu_shadow_transmit_texture"}};
  gpu::StaticShader alpha_prepass = {"mechazyiu_alpha_prepass"};
  gpu::StaticShader forward[2 /*light*/][2 /*texture*/][2 /*blend*/] = {
      {{{"mechazyiu_forward"}, {"mechazyiu_forward_blend"}},
       {{"mechazyiu_forward_texture"}, {"mechazyiu_forward_texture_blend"}}},
      {{{"mechazyiu_forward_light"}, {"mechazyiu_forward_light_blend"}},
       {{"mechazyiu_forward_light_texture"}, {"mechazyiu_forward_light_texture_blend"}}}};
  gpu::StaticShader shadow_mask[3] = {
      {"mechazyiu_shadow_mask_cube"}, {"mechazyiu_shadow_mask_spot"}, {"mechazyiu_shadow_mask_sun"}};
  gpu::StaticShader copy = {"mechazyiu_copy"};
  gpu::StaticShader output[2 /*bloom*/] = {{"mechazyiu_output"}, {"mechazyiu_output_bloom"}};
  gpu::StaticShader bloom_prefilter = {"mechazyiu_bloom_prefilter"};
  gpu::StaticShader bloom_downsample = {"mechazyiu_bloom_downsample"};
  gpu::StaticShader bloom_upsample = {"mechazyiu_bloom_upsample"};

  static gpu::StaticShaderCache<ShaderCache> &static_cache()
  {
    static gpu::StaticShaderCache<ShaderCache> cache;
    return cache;
  }
  static ShaderCache &get()
  {
    return static_cache().get();
  }
  static void release()
  {
    static_cache().release();
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tiles
 * \{ */

struct TilePlan {
  int2 size = int2(0);
  int2 tile = int2(0);
  int2 count = int2(0);

  int len() const
  {
    return count.x * count.y;
  }

  /** Pixel rectangle of a tile: x, y, width, height. */
  int4 rect(int index) const
  {
    const int2 cell(index % count.x, index / count.x);
    const int2 start = cell * tile;
    const int2 extent = math::min(tile, size - start);
    return int4(start.x, start.y, extent.x, extent.y);
  }
};

/**
 * Fewest tiles of at most `max_pixels` pixels and `max_tile_size` wide covering the image, as
 * square as possible.
 */
static TilePlan tile_plan_get(int2 size, int64_t max_pixels, int max_tile_size)
{
  TilePlan plan;
  plan.size = size;
  max_pixels = std::max<int64_t>(max_pixels, 256 * 256);
  for (int total = 1; total < 4096; total++) {
    int best_nx = 0;
    float best_aspect = FLT_MAX;
    for (int nx = 1; nx <= total; nx++) {
      if (total % nx != 0) {
        continue;
      }
      const int ny = total / nx;
      const int tw = (size.x + nx - 1) / nx;
      const int th = (size.y + ny - 1) / ny;
      if (tw > max_tile_size || th > max_tile_size || int64_t(tw) * th > max_pixels) {
        continue;
      }
      const float aspect = float(std::max(tw, th)) / float(std::max(1, std::min(tw, th)));
      if (aspect < best_aspect) {
        best_aspect = aspect;
        best_nx = nx;
      }
    }
    if (best_nx > 0) {
      plan.count = int2(best_nx, total / best_nx);
      plan.tile = int2((size.x + plan.count.x - 1) / plan.count.x,
                       (size.y + plan.count.y - 1) / plan.count.y);
      return plan;
    }
  }
  plan.count = int2((size.x + 255) / 256, (size.y + 255) / 256);
  plan.tile = int2(256);
  return plan;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Instance
 * \{ */

static bool mesh_has_uv_map(const Mesh &mesh)
{
  const StringRef active_uv_map = mesh.active_or_default_uv_map_name();
  if (active_uv_map.is_empty()) {
    return false;
  }
  if (mesh.runtime->wrapper_type == ME_WRAPPER_TYPE_BMESH) {
    const BMesh &bm = *mesh.runtime->edit_mesh->bm;
    const BMDataLayerLookup attr = BM_data_layer_lookup(bm, active_uv_map);
    return attr && bke::mesh::is_uv_map(bke::AttributeMetaData{attr.domain, attr.type});
  }
  return bke::mesh::is_uv_map(mesh.attributes().lookup_meta_data(active_uv_map));
}

/** Blackman-Harris window over [-0.5, 0.5] (the pixel filter of Cycles). */
static float blackman_harris(float x)
{
  const float t = 2.0f * float(M_PI) * (x + 0.5f);
  return math::max(0.0f,
                   0.35875f - 0.48829f * math::cos(t) + 0.14128f * math::cos(2.0f * t) -
                       0.01168f * math::cos(3.0f * t));
}

/**
 * Offset in [-0.5, 0.5] distributed like the Blackman-Harris window, for a uniform `u`: samples
 * are placed by the pixel filter and all weigh the same. (Weighting samples instead lets a few
 * of them dominate the mean, and the noise of the others never averages out.)
 */
static float blackman_harris_sample(float u)
{
  constexpr int table_len = 256;
  static const Array<float> inverse_cdf = []() {
    constexpr int steps = 4096;
    Array<float> cdf(steps + 1);
    cdf[0] = 0.0f;
    for (int i = 0; i < steps; i++) {
      cdf[i + 1] = cdf[i] + blackman_harris((float(i) + 0.5f) / steps - 0.5f);
    }
    Array<float> table(table_len + 1);
    int i = 0;
    for (int j = 0; j <= table_len; j++) {
      const float target = cdf[steps] * float(j) / table_len;
      while (i < steps && cdf[i + 1] < target) {
        i++;
      }
      const float range = cdf[i + 1] - cdf[i];
      const float f = range > 0.0f ? (target - cdf[i]) / range : 0.0f;
      table[j] = (float(i) + math::clamp(f, 0.0f, 1.0f)) / steps - 0.5f;
    }
    return table;
  }();
  const float x = math::clamp(u, 0.0f, 1.0f) * table_len;
  const int i = math::min(int(x), table_len - 1);
  return math::interpolate(inverse_cdf[i], inverse_cdf[i + 1], x - float(i));
}

/**
 * A surface with a blended material: drawn after the lights, back to front, over the sample.
 * One pass per side (back faces first) for the world light and emission, and one for the lights.
 */
struct BlendedDraw {
  PassMain base_ps[2] = {{"mechazyiu.blended"}, {"mechazyiu.blended_back"}};
  PassMain light_ps[2] = {{"mechazyiu.blended_light"}, {"mechazyiu.blended_light_back"}};
  bool double_sided = false;
  float3 bounds_min = float3(0.0f);
  float3 bounds_max = float3(0.0f);
  /** View space depth of the bounds' center, for the order. */
  float depth = 0.0f;
};

/** G-buffer shader of a surface: plain, base color image, all the images (needs tangents). */
enum SurfaceKind { SURFACE_PLAIN = 0, SURFACE_TEXTURE, SURFACE_MAPS };

/** One-hot selection of an image channel. */
static float4 channel_select(int channel)
{
  return float4(channel == 0, channel == 1, channel == 2, channel == 3);
}

/** What the GPU can do, chosen paths. */
struct Capabilities {
  /** Floating point render targets (16 and 32 bit): required. */
  bool supported = true;
  /** 4 render targets per draw, else the G-buffer is drawn once per target. */
  bool multiple_targets = true;
  /** Blending into floating point targets: lights are added by blending, else each light pass
   * reads the sample, writes the other buffer, and the rectangle is copied back. */
  bool float_blending = false;
  /** Normals in 10 bits per component when the hardware renders to it, else 8. */
  gpu::TextureFormat normal_format = gpu::TextureFormat::UNORM_10_10_10_2;
  int max_tile_size = 2048;

  static Capabilities get()
  {
    Capabilities caps;
    caps.supported = GPU_render_target_format_support(gpu::TextureFormat::SFLOAT_16_16_16_16) &&
                     GPU_render_target_format_support(gpu::TextureFormat::SFLOAT_32);
    caps.multiple_targets = GPU_max_color_targets() >= 4;
    caps.float_blending = GPU_float_blending_support();
    caps.normal_format = GPU_render_target_format_support(gpu::TextureFormat::UNORM_10_10_10_2) ?
                             gpu::TextureFormat::UNORM_10_10_10_2 :
                             gpu::TextureFormat::UNORM_8_8_8_8;
    caps.max_tile_size = GPU_max_texture_size();
    return caps;
  }
};

static const char *unsupported_message =
    "MECHAZYIU needs a GPU with floating point render targets (Radeon 9500, GeForce 6, Intel "
    "GMA X3000 or newer)";

class Instance : public DrawEngine {
 public:
  const DRWContext *draw_ctx = nullptr;

 private:
  Capabilities caps_ = Capabilities::get();
  View view_ = {"mechazyiu_view"};

  /* Scene. */
  const Scene *scene_ = nullptr;
  bool use_scene_lights_ = true;
  bool use_scene_world_ = true;
  bool is_render_ = false;
  int samples_len_ = 16;
  float filter_size_ = 1.5f;
  float light_threshold_ = 0.01f;
  bool transparent_ = false;
  /** Ambient occlusion (`Scene.mechazyiu`, a Python property group), distance 0: none. */
  float ao_distance_ = 0.0f;
  float ao_factor_ = 1.0f;
  BloomSettings bloom_;
  /** Viewport bloom: its levels (RGBM), the inputs of its passes, the last level's. */
  Texture bloom_bright_tx_ = {"mechazyiu.bloom_bright"};
  Texture bloom_down_tx_[bloom_levels_max];
  Texture bloom_up_tx_[bloom_levels_max];
  Framebuffer bloom_bright_fb_ = {"mechazyiu.bloom_bright"};
  Framebuffer bloom_down_fb_[bloom_levels_max];
  Framebuffer bloom_up_fb_[bloom_levels_max];
  gpu::Texture *bloom_src_ = nullptr;
  gpu::Texture *bloom_base_ = nullptr;
  gpu::Texture *bloom_out_ = nullptr;
  float4 bloom_texel_ = float4(0.0f);
  float4 bloom_curve_ = float4(0.0f);
  float4 bloom_params_ = float4(0.0f);
  float4 bloom_color_ = float4(0.0f);
  /** RGBM black (no bloom). */
  Texture black_tx_ = {"mechazyiu.black"};
  bool use_bounce_ = true;
  float4 mean_uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
  /** No occlusion nor bounce light. */
  Texture no_ao_tx_ = {"mechazyiu.no_ao"};
  float4 ao_params_ = float4(0.0f);
  float4 ao_taps_[2] = {};
  float4 ao_uv_to_view_ = float4(0.0f);
  /**
   * The sample is a viewport draft: the first after a change (every redraw while navigating),
   * without shadow maps nor ambient occlusion (most of the time of a sample). The next sample
   * replaces it.
   */
  bool draft_ = false;
  /** View pixels per pixel of the sample (drafts are rendered at half the resolution). */
  int draft_scale_ = 1;
  /** The ambient occlusion and bounce light of the sample, or `no_ao_tx_`. */
  gpu::Texture *ao_src_ = nullptr;
  /** Depth of field by moving the camera over its aperture (radius 0: none). */
  struct {
    float radius = 0.0f;
    float focus_distance = 1.0f;
    float blades = 0.0f;
    float rotation = 0.0f;
    float2 scale = float2(1.0f);
  } dof_;
  WorldLighting world_;
  Vector<LightData> lights_;
  Vector<MaterialData> materials_;
  Map<const Material *, int> material_indices_;
  /** G-buffer sub-passes of each material: [index][SurfaceKind][back faces]. */
  Map<int3, PassMain::Sub *> material_passes_;
  Set<gpu::Texture *> textures_;
  int64_t texture_bytes_ = 0;
  Vector<GPUMaterial *> dummy_gpu_materials_;

  /* Passes. */
  PassMain gbuffer_ps_ = {"mechazyiu.gbuffer"};
  PassSimple base_ps_ = {"mechazyiu.base"};
  PassSimple ao_ps_[2 /*ortho*/] = {{"mechazyiu.ao"}, {"mechazyiu.ao_ortho"}};
  PassSimple bounce_ps_ = {"mechazyiu.bounce"};
  PassSimple light_ps_[light_type_len] = {{"mechazyiu.light_point"},
                                           {"mechazyiu.light_spot"},
                                           {"mechazyiu.light_sun"},
                                           {"mechazyiu.light_area"}};
  PassSimple copy_ps_ = {"mechazyiu.copy"};
  PassSimple mix_ps_ = {"mechazyiu.mix"};
  PassSimple output_ps_[2 /*bloom*/] = {{"mechazyiu.output"}, {"mechazyiu.output_bloom"}};
  PassSimple bloom_prefilter_ps_ = {"mechazyiu.bloom_prefilter"};
  PassSimple bloom_downsample_ps_ = {"mechazyiu.bloom_downsample"};
  PassSimple bloom_upsample_ps_ = {"mechazyiu.bloom_upsample"};
  /** Shadow casters, perspective and orthographic. */
  PassMain shadow_ps_[2] = {{"mechazyiu.shadow"}, {"mechazyiu.shadow_ortho"}};
  /** Depth of the cutout surfaces, before the G-buffer. */
  PassMain alpha_prepass_ps_ = {"mechazyiu.alpha_prepass"};
  Map<int3, PassMain::Sub *> alpha_prepass_subs_;
  /** Cutout casters of the shadow passes, by material: [index][ortho]. */
  Map<int3, PassMain::Sub *> shadow_cutout_subs_;
  /** Transparent casters: their distance (perspective, orthographic), then their transmittance. */
  PassMain translucent_ps_[2] = {{"mechazyiu.translucent"}, {"mechazyiu.translucent_ortho"}};
  PassMain transmit_ps_ = {"mechazyiu.transmit"};
  Map<int3, PassMain::Sub *> transmit_subs_;
  bool has_translucent_casters_ = false;
  Vector<std::unique_ptr<BlendedDraw>> blended_;
  /** Shadow masks of cube, spot and sun shadow maps. */
  PassSimple shadow_mask_ps_[3] = {{"mechazyiu.shadow_mask_cube"},
                                    {"mechazyiu.shadow_mask_spot"},
                                    {"mechazyiu.shadow_mask_sun"}};
  View shadow_view_ = {"mechazyiu_shadow_view"};
  /** World space bounds of the shadow casters (sun shadow maps cover them). */
  float3 casters_min_ = float3(FLT_MAX);
  float3 casters_max_ = float3(-FLT_MAX);

  /* Tile buffers. */
  Texture gb_diffuse_tx_ = {"mechazyiu.gb_diffuse"};
  Texture gb_normal_tx_ = {"mechazyiu.gb_normal"};
  Texture gb_depth_tx_ = {"mechazyiu.gb_depth"};
  Texture gb_specular_tx_ = {"mechazyiu.gb_specular"};
  Texture depth_tx_ = {"mechazyiu.depth"};
  Texture accum_tx_[2] = {{"mechazyiu.accum0"}, {"mechazyiu.accum1"}};
  Framebuffer gbuffer_fb_ = {"mechazyiu.gbuffer"};
  /** One target per draw (no multiple render targets). */
  Framebuffer gbuffer_single_fb_[4] = {{"mechazyiu.gbuffer0"},
                                       {"mechazyiu.gbuffer1"},
                                       {"mechazyiu.gbuffer2"},
                                       {"mechazyiu.gbuffer3"}};
  /** Final renders: the mean of the tile's samples. */
  Texture tile_mean_tx_ = {"mechazyiu.tile_mean"};
  /** Dithered prepass: the depth, and a target for the alpha test (the shadow mask buffer). */
  Framebuffer alpha_prepass_fb_ = {"mechazyiu.alpha_prepass"};
  /** Blended surfaces over the sample, depth tested against the G-buffer. */
  Framebuffer forward_fb_[2] = {{"mechazyiu.forward0"}, {"mechazyiu.forward1"}};
  Framebuffer tile_mean_fb_ = {"mechazyiu.tile_mean"};
  Framebuffer accum_fb_[2] = {{"mechazyiu.accum0"}, {"mechazyiu.accum1"}};
  Texture shadow_mask_tx_ = {"mechazyiu.shadow_mask"};
  Framebuffer shadow_mask_fb_ = {"mechazyiu.shadow_mask"};

  /* Shadow maps. */
  Texture shadow_tx_ = {"mechazyiu.shadow"};
  Texture shadow_depth_tx_ = {"mechazyiu.shadow_depth"};
  Framebuffer shadow_fb_ = {"mechazyiu.shadow"};
  /** Transparent casters, at half the atlas resolution: the distance of the nearest one, the
   * light they let through. */
  Texture translucent_depth_tx_ = {"mechazyiu.translucent_depth"};
  Texture translucent_zbuf_tx_ = {"mechazyiu.translucent_zbuf"};
  Texture transmittance_tx_ = {"mechazyiu.transmittance"};
  Framebuffer translucent_fb_ = {"mechazyiu.translucent"};
  Framebuffer transmittance_fb_ = {"mechazyiu.transmittance"};
  /** Lights without shadow read this instead of the mask, and the shadow masks instead of the
   * transparent casters' maps when there are none. */
  Texture white_tx_ = {"mechazyiu.white"};
  /** Normal maps of materials without one: the unchanged normal. */
  Texture flat_normal_tx_ = {"mechazyiu.flat_normal"};
  gpu::Texture *translucent_depth_src_ = nullptr;
  gpu::Texture *transmittance_src_ = nullptr;
  TilePlan plan_;
  int2 tile_allocated_ = int2(0);
  int64_t budget_ = 0;
  /** Accumulation buffer holding the sample. */
  int cur_ = 0;

  /* Viewport: mean and linear depth of the whole view. */
  Texture mean_tx_ = {"mechazyiu.mean"};
  Texture view_depth_tx_ = {"mechazyiu.view_depth"};
  Framebuffer mean_fb_ = {"mechazyiu.mean"};
  Framebuffer view_depth_fb_ = {"mechazyiu.view_depth"};
  Framebuffer output_fb_ = {"mechazyiu.output"};

  Texture material_tx_ = {"mechazyiu.materials"};
  /** The environment (`world_.maps`, or 1 x 1 cubes of a uniform world's color). */
  Texture world_tx_[4] = {{"mechazyiu.world"},
                          {"mechazyiu.world_glossy"},
                          {"mechazyiu.world_rough"},
                          {"mechazyiu.world_irradiance"}};
  std::shared_ptr<const EnvironmentMaps> world_maps_;
  float3 world_maps_color_ = float3(-1.0f);
  /** View to environment direction (rows), see `mechazyiu_base`. */
  float4 world_rotation_[3] = {};
  float4 world_params_ = float4(0.0f);

  /* Values read by the passes when they are submitted. */
  float4 rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
  float4 proj_ = float4(0.0f);
  float4 proj_type_ = float4(0.0f);
  float4 world_sh_[4] = {};
  float4 gbuffer_target_ = float4(1.0f, 0.0f, 0.0f, 0.0f);
  gpu::Texture *forward_src_ = nullptr;
  float4 forward_light_position_ = float4(0.0f);
  float4 forward_light_direction_ = float4(0.0f);
  float4 forward_light_color_ = float4(0.0f);
  float4 forward_light_spot_ = float4(0.0f);
  /** Position of each light for the sample (or direction toward suns), world space. */
  Vector<float3> light_sample_positions_;
  float4 mix_weight_ = float4(1.0f);
  gpu::Texture *mean_src_ = nullptr;
  float4 light_position_ = float4(0.0f);
  float4 light_direction_ = float4(0.0f);
  float4 light_color_ = float4(0.0f);
  float4 light_spot_ = float4(0.0f);
  float4 uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
  float4 depth_proj_ = float4(0.0f);
  gpu::Texture *accum_src_ = nullptr;
  gpu::Texture *copy_src_ = nullptr;
  gpu::Texture *light_mask_src_ = nullptr;
  float4 shadow_light_ = float4(0.0f);
  float4 shadow_light_view_ = float4(0.0f);
  float4 shadow_mat_[4] = {};
  float4 shadow_params_ = float4(0.0f);
  float4 shadow_bias_ = float4(1.0f, 0.0f, 0.0f, 0.0f);

  /* Viewport progressive sampling. */
  int sample_ = 0;
  float weight_sum_ = 0.0f;
  uint64_t last_update_ = 0;
  float4x4 last_persmat_ = float4x4::zero();
  int2 last_size_ = int2(0);
  uint64_t last_settings_ = 0;

 public:
  StringRefNull name_get() final
  {
    return "MECHAZYIU";
  }

  void init() final
  {
    this->draw_ctx = DRW_context_get();
    this->init_scene();
  }

  void init_scene()
  {
    this->draw_ctx = DRW_context_get();
    scene_ = draw_ctx->scene;
    const View3D *v3d = draw_ctx->v3d;
    is_render_ = draw_ctx->is_image_render();
    use_scene_lights_ = v3d == nullptr || is_render_ || V3D_USES_SCENE_LIGHTS(v3d);
    use_scene_world_ = v3d == nullptr || is_render_ || V3D_USES_SCENE_WORLD(v3d);
    samples_len_ = is_render_ ? math::max(1, scene_->eevee.taa_render_samples) :
                                scene_->eevee.taa_samples;
    filter_size_ = scene_->r.gauss;
    light_threshold_ = scene_->eevee.light_threshold;
    transparent_ = use_scene_world_ && scene_->r.alphamode == R_ALPHAPREMUL;
    world_ = use_scene_world_ ? world_lighting_get(scene_->world) :
                                world_lighting_studio_get(*v3d, scene_->world);
    world_texture_ensure();
    dof_init();
    settings_read();
  }

  /** MECHAZYIU's settings of the scene: `Scene.mechazyiu` (defined in `properties_mechazyiu.py`). */
  void settings_read()
  {
    const IDProperty *group = scene_->id.system_properties ?
                                  IDP_GetPropertyTypeFromGroup(
                                      scene_->id.system_properties, "mechazyiu", IDP_GROUP) :
                                  nullptr;
    auto value = [&](const char *name, float fallback) {
      const IDProperty *prop = group ? IDP_GetPropertyFromGroup(group, name) : nullptr;
      if (prop == nullptr) {
        return fallback;
      }
      switch (prop->type) {
        case IDP_BOOLEAN:
          return IDP_bool_get(prop) ? 1.0f : 0.0f;
        case IDP_INT:
          return float(IDP_int_get(prop));
        default:
          return IDP_coerce_to_float_or_zero(prop);
      }
    };
    /* The defaults of the property group. */
    const bool use_ao = value("use_ambient_occlusion", 1.0f) != 0.0f;
    ao_distance_ = use_ao ? math::max(value("ambient_occlusion_distance", 0.2f), 0.0f) : 0.0f;
    ao_factor_ = value("ambient_occlusion_factor", 1.0f);
    use_bounce_ = value("use_bounce_light", 1.0f) != 0.0f;
    bloom_.enabled = value("use_bloom", 0.0f) != 0.0f;
    bloom_.threshold = value("bloom_threshold", 0.8f);
    bloom_.knee = value("bloom_knee", 0.5f);
    bloom_.radius = value("bloom_radius", 6.5f);
    bloom_.intensity = value("bloom_intensity", 0.05f);
    bloom_.clamp = value("bloom_clamp", 0.0f);
    bloom_.color = float3(1.0f);
    if (const IDProperty *color = group ? IDP_GetPropertyTypeFromGroup(group, "bloom_color", IDP_ARRAY) :
                                          nullptr)
    {
      if (color->subtype == IDP_FLOAT && color->len >= 3) {
        const float *values = static_cast<const float *>(IDP_array_voidp_get(color));
        bloom_.color = float3(values[0], values[1], values[2]);
      }
    }
  }

  /**
   * The 4 points of the ambient occlusion of a sample: turned and at other distances every sample
   * (the same for all pixels), so that the samples cover the disk around the pixels. Needs
   * `proj_` and `proj_type_`.
   */
  void ao_setup(int sample)
  {
    /* Texture coordinates to view space: the view rays are linear in normalized device
     * coordinates, uv = (ndc * 0.5 + 0.5) * proj_type.zw. */
    const float2 uv_to_view = 2.0f * float2(proj_.x, proj_.y) / float2(proj_type_.z, proj_type_.w);
    ao_uv_to_view_ = float4(uv_to_view, 0.0f, 0.0f);
    /* Twice the mean of the 4 points: surrounded pixels (half of the points below the surface)
     * are fully occluded. */
    const float factor = math::max(ao_factor_, 0.0f) / 2.0f;
    ao_params_ = float4(1.0f / math::square(ao_distance_),
                        factor,
                        use_bounce_ ? factor : 0.0f,
                        proj_type_.x > 0.5f ? 1.0f : 0.0f);
    const float angle = halton(sample, 23) * 2.0f * float(M_PI);
    const float shift = halton(sample, 29);
    float2 taps[4];
    for (int i = 0; i < 4; i++) {
      const float a = angle + float(i) * float(M_PI_2);
      const float radius = (float(i) + shift) / 4.0f * ao_distance_;
      taps[i] = float2(math::cos(a), math::sin(a)) * radius / uv_to_view;
    }
    ao_taps_[0] = float4(taps[0], taps[1]);
    ao_taps_[1] = float4(taps[2], taps[3]);
  }

  /** The camera's depth of field, like EEVEE's (renders, and the viewport through the camera). */
  void dof_init()
  {
    dof_.radius = 0.0f;
    const RegionView3D *rv3d = draw_ctx->rv3d;
    const Object *camera_object = nullptr;
    if (is_render_) {
      camera_object = scene_->camera;
    }
    else if (draw_ctx->v3d && rv3d && rv3d->persp == RV3D_CAMOB && draw_ctx->v3d->camera) {
      camera_object = DEG_get_evaluated(draw_ctx->depsgraph, draw_ctx->v3d->camera);
    }
    if (camera_object == nullptr || camera_object->type != OB_CAMERA) {
      return;
    }
    const Camera &camera = *reinterpret_cast<const Camera *>(camera_object->data);
    if ((camera.dof.flag & CAM_DOF_ENABLED) == 0 || camera.type == CAM_PANO) {
      return;
    }
    float aperture = 1.0f / (2.0f * math::max(camera.dof.aperture_fstop, 1e-5f));
    aperture *= (camera.type == CAM_ORTHO) ? 0.04f : camera.lens * 1e-3f;
    const float ratio = camera.dof.aperture_ratio;
    if (ratio < 1.0f) {
      aperture /= math::max(ratio, 1e-5f);
    }
    dof_.radius = aperture;
    dof_.focus_distance = BKE_camera_object_dof_distance(camera_object);
    dof_.blades = float(camera.dof.aperture_blades);
    dof_.rotation = camera.dof.aperture_rotation;
    dof_.scale = float2(math::clamp(1.0f / ratio, 1e-5f, 1.0f), math::clamp(ratio, 1e-5f, 1.0f));
  }

  /**
   * Moves the camera to a point of the aperture for a sample (the first one at the center), the
   * projection skewed so that the focus plane stays (EEVEE's `DepthOfField::jitter_apply`).
   */
  void dof_jitter(int sample, float4x4 &viewmat, float4x4 &winmat) const
  {
    if (dof_.radius == 0.0f || sample == 0) {
      return;
    }
    /* Uniform over the disk, or the polygon of the blades. */
    float radius = math::sqrt(halton(sample, 17));
    float theta = halton(sample, 19) * 2.0f * float(M_PI);
    if (dof_.blades >= 3.0f) {
      const float side_angle = 2.0f * float(M_PI) / dof_.blades;
      auto polygon_radius = [&](float angle) {
        return math::cos(side_angle * 0.5f) /
               math::cos(angle - side_angle * math::floor((dof_.blades * angle + float(M_PI)) /
                                                          (2.0f * float(M_PI))));
      };
      /* Even spacing along the sides. */
      const float side = math::floor(theta / side_angle);
      const float ratio = (theta - side * side_angle - side_angle * 0.5f) / (side_angle * 0.5f);
      theta = side * side_angle + math::atan(ratio * math::sin(float(M_PI) / dof_.blades) /
                                             polygon_radius(0.0f));
      radius *= polygon_radius(theta);
    }
    radius *= dof_.radius;
    theta += dof_.rotation;
    float2 offset = float2(radius * math::cos(theta), radius * math::sin(theta)) * dof_.scale;

    float3 jitter(offset, -dof_.focus_distance);
    float3 center(0.0f, 0.0f, -dof_.focus_distance);
    jitter = math::project_point(winmat, jitter);
    center = math::project_point(winmat, center);
    if (winmat[2][3] != -1.0f) {
      offset *= dof_.focus_distance;
    }
    viewmat[3].x -= offset.x;
    viewmat[3].y -= offset.y;
    winmat[2].x += center.x - jitter.x;
    winmat[2].y += center.y - jitter.y;
  }

  /** The sun extracted from the world, a sun light (not for environments turning with the view). */
  void world_sun_add()
  {
    if (math::reduce_max(world_.sun_irradiance) <= 0.0f || world_.view_space) {
      return;
    }
    /* Environment to world space: the environment's rotation around Z. */
    const float c = math::cos(world_.rotation_z), s = math::sin(world_.rotation_z);
    const float3 d = world_.sun_direction;
    const float3 z = math::normalize(float3(d.x * c - d.y * s, d.x * s + d.y * c, d.z));
    const float3 x = math::normalize(
        math::cross(math::abs(z.z) < 0.9f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f),
                    z));
    LightData sun;
    sun.type = LightType::SUN;
    sun.object_to_world = float4x4(float4(x, 0.0f),
                                   float4(math::cross(z, x), 0.0f),
                                   float4(z, 0.0f),
                                   float4(0.0f, 0.0f, 0.0f, 1.0f));
    sun.intensity = world_.sun_irradiance;
    sun.radius = world_.sun_angle;
    sun.shadow = world_.sun_shadow;
    lights_.append(sun);
  }

  void begin_sync() final
  {
    lights_.clear();
    world_sun_add();
    materials_.clear();
    material_indices_.clear();
    material_passes_.clear();
    textures_.clear();
    texture_bytes_ = 0;

    gbuffer_ps_.init();
    gbuffer_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                          DRW_STATE_DEPTH_LESS_EQUAL);
    alpha_prepass_ps_.init();
    alpha_prepass_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                                DRW_STATE_DEPTH_LESS_EQUAL);
    alpha_prepass_subs_.clear();
    shadow_cutout_subs_.clear();
    transmit_subs_.clear();
    has_translucent_casters_ = false;
    for (int ortho = 0; ortho < 2; ortho++) {
      translucent_ps_[ortho].init();
      translucent_ps_[ortho].state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                                       DRW_STATE_DEPTH_LESS_EQUAL);
      translucent_ps_[ortho].shader_set(ShaderCache::get().shadow[ortho].get());
      translucent_ps_[ortho].push_constant("shadow_light", &shadow_light_);
    }
    transmit_ps_.init();
    transmit_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_MUL);
    blended_.clear();

    ShaderCache &shaders = ShaderCache::get();
    auto screen_pass = [&](PassSimple &ps, gpu::Shader *shader) {
      ps.init();
      ps.state_set(DRW_STATE_WRITE_COLOR);
      ps.shader_set(shader);
      ps.push_constant("rect", &rect_);
      ps.push_constant("proj", &proj_);
      ps.push_constant("proj_type", &proj_type_);
    };
    auto gbuffer_read = [&](PassSimple &ps) {
      ps.bind_texture("gb_diffuse_tx", &gb_diffuse_tx_);
      ps.bind_texture("gb_normal_tx", &gb_normal_tx_);
      ps.bind_texture("gb_depth_tx", &gb_depth_tx_);
      ps.bind_texture("gb_specular_tx", &gb_specular_tx_);
    };

    for (int ortho = 0; ortho < 2; ortho++) {
      PassSimple &ps = ao_ps_[ortho];
      screen_pass(ps, shaders.ao[ortho].get());
      gbuffer_read(ps);
      ps.push_constant("ao_params", &ao_params_);
      ps.push_constant("ao_taps", ao_taps_, 2);
      ps.push_constant("ao_uv_to_view", &ao_uv_to_view_);
      ps.draw_procedural(GPU_PRIM_TRIS, 1, 6);
    }
    screen_pass(bounce_ps_, shaders.bounce.get());
    gbuffer_read(bounce_ps_);
    bounce_ps_.push_constant("ao_params", &ao_params_);
    bounce_ps_.push_constant("ao_taps", ao_taps_, 2);
    bounce_ps_.push_constant("mean_uv_transform", &mean_uv_transform_);
    bounce_ps_.bind_texture("weights_tx", &shadow_mask_tx_);
    bounce_ps_.bind_texture("mean_tx", &mean_src_);
    bounce_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);

    screen_pass(base_ps_, shaders.base.get());
    base_ps_.bind_texture("ao_tx", &ao_src_);
    gbuffer_read(base_ps_);
    base_ps_.bind_texture("material_tx", &material_tx_);
    {
      const GPUSamplerState sampler = {GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP,
                                       GPU_SAMPLER_EXTEND_MODE_EXTEND,
                                       GPU_SAMPLER_EXTEND_MODE_EXTEND};
      const char *names[4] = {"world_tx", "world_glossy_tx", "world_rough_tx", "irradiance_tx"};
      for (int i = 0; i < 4; i++) {
        base_ps_.bind_texture(names[i], &world_tx_[i], sampler);
      }
    }
    base_ps_.push_constant("world_rotation", world_rotation_, 3);
    base_ps_.push_constant("world_params", &world_params_);
    base_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);

    for (int type = 0; type < light_type_len; type++) {
      PassSimple &ps = light_ps_[type];
      screen_pass(ps, shaders.light[caps_.float_blending][type].get());
      if (caps_.float_blending) {
        ps.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ADD_FULL);
      }
      else {
        ps.bind_texture("accum_tx", &accum_src_);
      }
      gbuffer_read(ps);
      ps.push_constant("light_position", &light_position_);
      ps.push_constant("light_direction", &light_direction_);
      ps.push_constant("light_color", &light_color_);
      ps.push_constant("light_spot", &light_spot_);
      ps.bind_texture("shadow_mask_tx", &light_mask_src_);
      ps.draw_procedural(GPU_PRIM_TRIS, 1, 6);
    }

    for (int ortho = 0; ortho < 2; ortho++) {
      PassMain &ps = shadow_ps_[ortho];
      ps.init();
      ps.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL);
      ps.shader_set(shaders.shadow[ortho].get());
      ps.push_constant("shadow_light", &shadow_light_);
    }
    for (int kind = 0; kind < 3; kind++) {
      PassSimple &ps = shadow_mask_ps_[kind];
      screen_pass(ps, shaders.shadow_mask[kind].get());
      ps.bind_texture("gb_normal_tx", &gb_normal_tx_);
      ps.bind_texture("gb_depth_tx", &gb_depth_tx_);
      /* Packed distances: never filtered. */
      ps.bind_texture("shadow_tx", &shadow_tx_, GPUSamplerState::default_sampler());
      ps.bind_texture(
          "translucent_depth_tx", &translucent_depth_src_, GPUSamplerState::default_sampler());
      ps.bind_texture("transmittance_tx", &transmittance_src_);
      ps.push_constant("light_position", &shadow_light_view_);
      ps.push_constant("shadow_mat", shadow_mat_, 4);
      ps.push_constant("shadow_params", &shadow_params_);
      ps.push_constant("shadow_bias", &shadow_bias_);
      ps.draw_procedural(GPU_PRIM_TRIS, 1, 6);
    }
    casters_min_ = float3(FLT_MAX);
    casters_max_ = float3(-FLT_MAX);

    screen_pass(copy_ps_, shaders.copy.get());
    copy_ps_.bind_texture("source_tx", &copy_src_);
    copy_ps_.push_constant("uv_transform", &uv_transform_);
    copy_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);

    screen_pass(mix_ps_, shaders.mix.get());
    mix_ps_.bind_texture("source_tx", &copy_src_);
    mix_ps_.bind_texture("mean_tx", &mean_src_);
    mix_ps_.push_constant("uv_transform", &uv_transform_);
    mix_ps_.push_constant("mix_weight", &mix_weight_);
    mix_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);

    const GPUSamplerState bloom_sampler = {GPU_SAMPLER_FILTERING_LINEAR,
                                           GPU_SAMPLER_EXTEND_MODE_EXTEND,
                                           GPU_SAMPLER_EXTEND_MODE_EXTEND};
    screen_pass(bloom_prefilter_ps_, shaders.bloom_prefilter.get());
    bloom_prefilter_ps_.bind_texture("source_tx", &bloom_src_, bloom_sampler);
    bloom_prefilter_ps_.push_constant("bloom_texel", &bloom_texel_);
    bloom_prefilter_ps_.push_constant("bloom_curve", &bloom_curve_);
    bloom_prefilter_ps_.push_constant("bloom_params", &bloom_params_);
    bloom_prefilter_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);
    screen_pass(bloom_downsample_ps_, shaders.bloom_downsample.get());
    bloom_downsample_ps_.bind_texture("source_tx", &bloom_src_, bloom_sampler);
    bloom_downsample_ps_.push_constant("bloom_texel", &bloom_texel_);
    bloom_downsample_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);
    screen_pass(bloom_upsample_ps_, shaders.bloom_upsample.get());
    bloom_upsample_ps_.bind_texture("source_tx", &bloom_src_, bloom_sampler);
    bloom_upsample_ps_.bind_texture("base_tx", &bloom_base_, bloom_sampler);
    bloom_upsample_ps_.push_constant("bloom_texel", &bloom_texel_);
    bloom_upsample_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);

    for (int bloom = 0; bloom < 2; bloom++) {
      PassSimple &ps = output_ps_[bloom];
      screen_pass(ps, shaders.output[bloom].get());
      ps.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_ALWAYS);
      ps.bind_texture("source_tx", &mean_tx_);
      ps.bind_texture("depth_tx", &view_depth_tx_);
      ps.push_constant("uv_transform", &uv_transform_);
      ps.push_constant("depth_proj", &depth_proj_);
      if (bloom) {
        ps.bind_texture("bloom_tx", &bloom_out_, bloom_sampler);
        ps.push_constant("bloom_color", &bloom_color_);
      }
      ps.draw_procedural(GPU_PRIM_TRIS, 1, 6);
    }
  }

  /** Index of the material in the table, its data is read on first use. */
  int material_index_get(const Material *material)
  {
    return material_indices_.lookup_or_add_cb(material, [&]() {
      MaterialData data = material_data_get(material);
      for (gpu::Texture **texture : {data.texture,
                                     data.normal_map.texture,
                                     data.roughness_map.texture,
                                     data.metallic_map.texture,
                                     data.coverage_map.texture})
      {
        if (texture && *texture && textures_.add(*texture)) {
          texture_bytes_ += int64_t(GPU_texture_width(*texture)) * GPU_texture_height(*texture) *
                            4 * 4 / 3;
        }
      }
      materials_.append(data);
      return int(materials_.size() - 1);
    });
  }

  PassMain::Sub &material_pass_get(int index, SurfaceKind kind, bool back)
  {
    return *material_passes_.lookup_or_add_cb(int3(index, kind, back), [&]() {
      const MaterialData &data = materials_[index];
      PassMain::Sub &sub = gbuffer_ps_.sub("mechazyiu.material");
      /* Cutouts: only the pixels their prepass kept. */
      const DRWState depth_test = data.cutout() && kind != SURFACE_PLAIN ?
                                      DRW_STATE_DEPTH_EQUAL :
                                      DRW_STATE_DEPTH_LESS_EQUAL;
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | depth_test |
                    (back ? DRW_STATE_CULL_FRONT : DRW_STATE_CULL_BACK));
      const bool single = !caps_.multiple_targets;
      sub.shader_set(ShaderCache::get().gbuffer[kind][back][single].get());
      if (single) {
        sub.push_constant("gbuffer_target", &gbuffer_target_);
      }
      sub.push_constant("mat_base", float4(data.base_color, data.alpha));
      const float table_index = float(math::min(index, material_table_len - 1)) / 255.0f;
      sub.push_constant(
          "mat_params",
          float4(data.metallic, math::clamp(data.roughness, 0.045f, 1.0f), data.f0, table_index));
      if (kind != SURFACE_PLAIN) {
        if (data.texture && *data.texture) {
          sub.bind_texture("base_tx", data.texture, data.sampler);
        }
        else {
          /* Cutouts of an image used only for the alpha. */
          sub.bind_texture("base_tx", &white_tx_);
        }
      }
      if (kind == SURFACE_MAPS) {
        auto bind_map = [&](const char *name, const MapTexture &map, Texture &fallback) {
          if (map.valid()) {
            sub.bind_texture(name, map.texture, map.sampler);
          }
          else {
            sub.bind_texture(name, &fallback);
          }
        };
        bind_map("normal_tx", data.normal_map, flat_normal_tx_);
        bind_map("roughness_tx", data.roughness_map, white_tx_);
        bind_map("metallic_tx", data.metallic_map, white_tx_);
        sub.push_constant("roughness_channel", channel_select(data.roughness_map.channel));
        sub.push_constant("metallic_channel", channel_select(data.metallic_map.channel));
        sub.push_constant("mat_maps", float4(data.normal_strength, 0.0f, 0.0f, 0.0f));
      }
      return &sub;
    });
  }

  /** Cutout surfaces: depth where the image alpha is 0.5 or more. */
  PassMain::Sub &alpha_prepass_get(int index)
  {
    const MaterialData &data = materials_[index];
    return *alpha_prepass_subs_.lookup_or_add_cb(int3(index, 0, 0), [&]() {
      PassMain::Sub &sub = alpha_prepass_ps_.sub("mechazyiu.alpha_prepass");
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                    (data.backface_culling ? DRW_STATE_CULL_BACK : DRWState(0)));
      sub.shader_set(ShaderCache::get().alpha_prepass.get());
      sub.bind_texture("base_tx", data.coverage_map.texture, data.coverage_map.sampler);
      sub.push_constant("coverage_channel", channel_select(data.coverage_map.channel));
      return &sub;
    });
  }

  /** Cutout shadow casters. */
  PassMain::Sub &shadow_cutout_get(int index, bool ortho)
  {
    const MaterialData &data = materials_[index];
    return *shadow_cutout_subs_.lookup_or_add_cb(int3(index, ortho, 0), [&]() {
      PassMain::Sub &sub = shadow_ps_[ortho].sub("mechazyiu.shadow_cutout");
      sub.shader_set(ShaderCache::get().shadow_cutout[ortho].get());
      sub.push_constant("shadow_light", &shadow_light_);
      sub.bind_texture("base_tx", data.coverage_map.texture, data.coverage_map.sampler);
      sub.push_constant("coverage_channel", channel_select(data.coverage_map.channel));
      return &sub;
    });
  }

  /** Transparent shadow casters: the light they let through. */
  PassMain::Sub &transmit_get(int index, bool textured)
  {
    const MaterialData &data = materials_[index];
    return *transmit_subs_.lookup_or_add_cb(int3(index, textured, 0), [&]() {
      PassMain::Sub &sub = transmit_ps_.sub("mechazyiu.transmit");
      sub.shader_set(ShaderCache::get().shadow_transmit[textured].get());
      sub.push_constant("shadow_light", &shadow_light_);
      sub.push_constant("mat_base", float4(data.base_color, data.alpha));
      sub.push_constant("mat_params", float4(0.0f, 0.0f, 0.0f, data.texture_alpha ? 1.0f : 0.0f));
      if (textured) {
        sub.bind_texture("base_tx", data.texture, data.sampler);
      }
      return &sub;
    });
  }

  /** Passes of a surface with a blended material, see #BlendedDraw. */
  void blended_sync(const MaterialData &data,
                    bool textured,
                    gpu::Batch *batch,
                    ResourceHandleRange handle,
                    const Object &ob)
  {
    std::unique_ptr<BlendedDraw> draw = std::make_unique<BlendedDraw>();
    draw->double_sided = !data.backface_culling;
    const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(&ob);
    draw->bounds_min = float3(FLT_MAX);
    draw->bounds_max = float3(-FLT_MAX);
    for (int corner = 0; corner < 8 && bounds; corner++) {
      const float3 local((corner & 1) ? bounds->max.x : bounds->min.x,
                         (corner & 2) ? bounds->max.y : bounds->min.y,
                         (corner & 4) ? bounds->max.z : bounds->min.z);
      const float3 world = math::transform_point(ob.object_to_world(), local);
      draw->bounds_min = math::min(draw->bounds_min, world);
      draw->bounds_max = math::max(draw->bounds_max, world);
    }
    if (!bounds) {
      draw->bounds_min = draw->bounds_max = ob.object_to_world().location();
    }
    /* Blinn-Phong exponent matching the roughness. */
    const float a = math::square(math::clamp(data.roughness, 0.045f, 1.0f));
    const float exponent = math::clamp(2.0f / (a * a) - 2.0f, 1.0f, 2048.0f);
    const float4 mat_params(data.metallic, exponent, data.f0, data.texture_alpha ? 1.0f : 0.0f);
    const bool hw_blend = caps_.float_blending;
    ShaderCache &shaders = ShaderCache::get();
    for (int back = 0; back < 2; back++) {
      const DRWState cull = back ? DRW_STATE_CULL_FRONT : DRW_STATE_CULL_BACK;
      for (int light = 0; light < 2; light++) {
        PassMain &ps = light ? draw->light_ps[back] : draw->base_ps[back];
        ps.init();
        DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | cull;
        if (hw_blend) {
          state |= light ? DRW_STATE_BLEND_ADD_FULL : DRW_STATE_BLEND_ALPHA_PREMUL;
        }
        ps.state_set(state);
        ps.shader_set(shaders.forward[light][textured][hw_blend].get());
        ps.push_constant("mat_base", float4(data.base_color, data.alpha));
        ps.push_constant("mat_params", mat_params);
        ps.push_constant("proj_type", &proj_type_);
        if (!hw_blend) {
          ps.bind_texture("sample_tx", &forward_src_);
        }
        if (textured) {
          ps.bind_texture("base_tx", data.texture, data.sampler);
        }
        if (light) {
          ps.push_constant("light_position", &forward_light_position_);
          ps.push_constant("light_direction", &forward_light_direction_);
          ps.push_constant("light_color", &forward_light_color_);
          ps.push_constant("light_spot", &forward_light_spot_);
        }
        else {
          ps.push_constant("world_sh", world_sh_, 4);
          ps.push_constant("emission", float4(data.emission, 0.0f));
        }
        ps.draw(batch, handle);
      }
    }
    blended_.append(std::move(draw));
  }

  void object_sync(ObjectRef &ob_ref, Manager &manager) final
  {
    Object *ob = ob_ref.object;
    if (ob->type == OB_LAMP) {
      if (use_scene_lights_ && (DRW_object_visibility_in_active_context(ob) & OB_VISIBLE_SELF)) {
        lights_.append(light_data_get(*ob, light_threshold_));
      }
      return;
    }
    if (!DRW_object_is_renderable(ob)) {
      return;
    }
    if (!(DRW_object_visibility_in_active_context(ob) & OB_VISIBLE_SELF) ||
        (ob->dt < OB_SOLID && !draw_ctx->is_scene_render()))
    {
      return;
    }
    if (!ELEM(ob->type, OB_MESH, OB_CURVES_LEGACY, OB_SURF, OB_FONT, OB_MBALL)) {
      return;
    }

    const int material_len = BKE_object_material_used_with_fallback_eval(*ob);
    const bool has_uv = ob->type == OB_MESH &&
                        mesh_has_uv_map(DRW_object_get_data_for_drawing<Mesh>(*ob));
    /* Images beyond the base color need the tangents. */
    bool use_maps = false;
    if (has_uv) {
      for (const int i : IndexRange(material_len)) {
        const int index = material_index_get(BKE_object_material_get_eval(ob, i + 1));
        use_maps |= materials_[index].has_maps() && !materials_[index].transparent();
      }
    }
    Span<gpu::Batch *> batches;
    if (use_maps) {
      batches = DRW_cache_mesh_surface_texpaint_tangent_get(ob);
    }
    else if (has_uv) {
      batches = DRW_cache_mesh_surface_texpaint_get(ob);
    }
    else {
      if (dummy_gpu_materials_.size() < material_len) {
        dummy_gpu_materials_.resize(material_len, nullptr);
      }
      batches = DRW_cache_object_surface_material_get(
          ob, dummy_gpu_materials_.as_span().slice(IndexRange(material_len)));
    }
    if (batches.is_empty()) {
      return;
    }

    ResourceHandleRange handle = manager.unique_handle(ob_ref);
    const bool casts_shadow = (ob->visibility_flag & OB_HIDE_SHADOW) == 0;
    if (casts_shadow) {
      if (const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(ob)) {
        for (int corner = 0; corner < 8; corner++) {
          const float3 local((corner & 1) ? bounds->max.x : bounds->min.x,
                             (corner & 2) ? bounds->max.y : bounds->min.y,
                             (corner & 4) ? bounds->max.z : bounds->min.z);
          const float3 world = math::transform_point(ob->object_to_world(), local);
          casters_min_ = math::min(casters_min_, world);
          casters_max_ = math::max(casters_max_, world);
        }
      }
    }
    for (const int i : IndexRange(math::min(material_len, int(batches.size())))) {
      if (batches[i] == nullptr) {
        continue;
      }
      const Material *material = BKE_object_material_get_eval(ob, i + 1);
      const int index = material_index_get(material);
      const MaterialData &data = materials_[index];
      const bool textured = has_uv && data.texture && *data.texture;
      const bool cutout = data.cutout() && has_uv;
      const SurfaceKind kind = (use_maps && data.has_maps()) ? SURFACE_MAPS :
                               (textured || cutout)          ? SURFACE_TEXTURE :
                                                               SURFACE_PLAIN;
      if (casts_shadow) {
        if (data.transparent()) {
          translucent_ps_[0].draw(batches[i], handle);
          translucent_ps_[1].draw(batches[i], handle);
          transmit_get(index, textured).draw(batches[i], handle);
          has_translucent_casters_ = true;
        }
        else {
          for (int ortho = 0; ortho < 2; ortho++) {
            if (cutout) {
              shadow_cutout_get(index, ortho).draw(batches[i], handle);
            }
            else {
              shadow_ps_[ortho].draw(batches[i], handle);
            }
          }
        }
      }
      if (data.transparent()) {
        blended_sync(data, textured, batches[i], handle, *ob);
        continue;
      }
      if (cutout) {
        alpha_prepass_get(index).draw(batches[i], handle);
      }
      material_pass_get(index, kind, false).draw(batches[i], handle);
      if (!data.backface_culling) {
        material_pass_get(index, kind, true).draw(batches[i], handle);
      }
    }
  }

  void end_sync() final
  {
    /* Emission of each material, read by the base pass. */
    Array<float4> table(material_table_len, float4(0.0f));
    for (const int i : materials_.index_range().take_front(material_table_len)) {
      table[i] = float4(materials_[i].emission, 0.0f);
    }
    material_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32,
                           int2(material_table_len, 1),
                           GPU_TEXTURE_USAGE_SHADER_READ);
    GPU_texture_update(material_tx_, GPU_DATA_FLOAT, table.data());
    if (flat_normal_tx_.gpu_texture() == nullptr) {
      const float4 flat(0.5f, 0.5f, 1.0f, 1.0f);
      flat_normal_tx_.ensure_2d(
          gpu::TextureFormat::UNORM_8_8_8_8, int2(1), GPU_TEXTURE_USAGE_SHADER_READ, flat);
    }
    if (white_tx_.gpu_texture() == nullptr) {
      const float4 white(1.0f);
      white_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8,
                          int2(1),
                          GPU_TEXTURE_USAGE_SHADER_READ,
                          white);
    }
    if (black_tx_.gpu_texture() == nullptr) {
      const float4 black(0.0f);
      black_tx_.ensure_2d(
          gpu::TextureFormat::UNORM_8_8_8_8, int2(1), GPU_TEXTURE_USAGE_SHADER_READ, black);
    }
    if (no_ao_tx_.gpu_texture() == nullptr) {
      const float4 none(0.0f, 0.0f, 0.0f, 1.0f);
      no_ao_tx_.ensure_2d(
          gpu::TextureFormat::UNORM_8_8_8_8, int2(1), GPU_TEXTURE_USAGE_SHADER_READ, none);
    }
  }

  /* ------------------------------------------------------------------ */
  /* Memory. */

  /**
   * Choose the tiles and (re)allocate their buffers so they fit the free video memory. Render
   * targets beyond it would be placed in AGP memory by the driver (5 times slower on the Radeon
   * 9600): their write rate is measured, and the tiles shrink when some were.
   */
  /** Uploads the environment maps when they changed (they stay the same while cached). */
  void world_texture_ensure()
  {
    std::shared_ptr<const EnvironmentMaps> maps = world_.maps;
    if (maps) {
      if (maps == world_maps_ && world_tx_[0].is_valid()) {
        return;
      }
      world_maps_color_ = float3(-1.0f);
    }
    else {
      /* Uniform world: its color is the cubes', strength 1. */
      if (!world_maps_ && world_tx_[0].is_valid() &&
          math::is_equal(world_maps_color_, world_.background, 0.0f))
      {
        return;
      }
      world_maps_color_ = world_.background;
      maps = environment_maps_uniform(world_.background);
    }
    world_maps_ = world_.maps;
    for (int i = 0; i < 4; i++) {
      const EnvironmentCube &cube = maps->cubes[i];
      world_tx_[i].free();
      world_tx_[i].ensure_cube(gpu::TextureFormat::UNORM_8_8_8_8,
                               cube.size,
                               GPU_TEXTURE_USAGE_SHADER_READ,
                               nullptr,
                               cube.levels.size());
      for (const int level : cube.levels.index_range()) {
        GPU_texture_update_mipmap(world_tx_[i], level, GPU_DATA_UBYTE, cube.levels[level].data());
      }
    }
  }

  void tiles_ensure(int2 size, bool viewport)
  {
    int64_t available = GPU_render_target_budget();
    const bool budget_known = available >= 0;
    if (!budget_known) {
      available = int64_t(96) << 20;
    }
    /* What MECHAZYIU holds already is available to it. */
    available += int64_t(tile_allocated_.x) * tile_allocated_.y * tile_bytes_per_pixel;
    if (viewport) {
      if (mean_tx_.gpu_texture()) {
        available += int64_t(GPU_texture_width(mean_tx_)) * GPU_texture_height(mean_tx_) *
                     view_bytes_per_pixel;
      }
      available -= int64_t(size.x) * size.y * view_bytes_per_pixel;
    }
    /* The shadow maps, and video memory for the textures of the scene too. */
    if (shadow_tx_.gpu_texture() == nullptr) {
      available -= shadow_atlas_bytes;
    }
    available = math::max(available - texture_bytes_, int64_t(4) << 20);

    if (viewport) {
      const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
      if (mean_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, size, usage) |
          view_depth_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32, size, usage))
      {
        mean_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(mean_tx_));
        view_depth_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(view_depth_tx_));
        /* A new mean starts over. */
        sample_ = 0;
      }
    }

    /* The budget changes a little with what else is allocated: keep tiles that still fit. */
    if (size == plan_.size && plan_.tile == tile_allocated_ &&
        int64_t(tile_allocated_.x) * tile_allocated_.y * tile_bytes_per_pixel <=
            available + available / 8)
    {
      return;
    }
    TilePlan plan = tile_plan_get(size, available / tile_bytes_per_pixel, caps_.max_tile_size);
    for (int attempt = 0; attempt < 4; attempt++) {
      if (plan.tile != tile_allocated_ || attempt == 0) {
        tile_buffers_free();
        /* Let the render targets take the video memory before the textures (restored later from
         * their system memory copies). */
        GPU_memory_evict_managed();
        tile_buffers_alloc(plan.tile);
      }
      if (!budget_known || tile_buffers_in_video_memory()) {
        break;
      }
      available = available * 3 / 5;
      plan = tile_plan_get(size, available / tile_bytes_per_pixel, caps_.max_tile_size);
    }
    plan_ = plan;
    budget_ = available;
    printf("MECHAZYIU: %dx%d in %d tile%s of %dx%d (%d MB of video memory for them)\n",
           size.x,
           size.y,
           plan.len(),
           plan.len() > 1 ? "s" : "",
           plan.tile.x,
           plan.tile.y,
           int(int64_t(plan.tile.x) * plan.tile.y * tile_bytes_per_pixel >> 20));
    fflush(stdout);
  }

  void tile_buffers_alloc(int2 size)
  {
    const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
    gb_diffuse_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, size, usage);
    gb_normal_tx_.ensure_2d(caps_.normal_format, size, usage);
    gb_depth_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32, size, usage);
    gb_specular_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, size, usage);
    shadow_mask_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, size, usage);
    shadow_mask_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(shadow_mask_tx_));
    const int2 atlas_size(shadow_atlas_width, shadow_atlas_height);
    shadow_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, atlas_size, usage);
    shadow_depth_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8, atlas_size, usage);
    shadow_fb_.ensure(GPU_ATTACHMENT_TEXTURE(shadow_depth_tx_), GPU_ATTACHMENT_TEXTURE(shadow_tx_));
    const int2 half_atlas = atlas_size / 2;
    translucent_depth_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, half_atlas, usage);
    translucent_zbuf_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8, half_atlas, usage);
    transmittance_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, half_atlas, usage);
    translucent_fb_.ensure(GPU_ATTACHMENT_TEXTURE(translucent_zbuf_tx_),
                           GPU_ATTACHMENT_TEXTURE(translucent_depth_tx_));
    transmittance_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(transmittance_tx_));
    depth_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8, size, usage);
    for (int i = 0; i < 2; i++) {
      accum_tx_[i].ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, size, usage);
      accum_fb_[i].ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(accum_tx_[i]));
    }
    tile_mean_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, size, usage);
    tile_mean_fb_.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(tile_mean_tx_));
    alpha_prepass_fb_.ensure(GPU_ATTACHMENT_TEXTURE(depth_tx_),
                             GPU_ATTACHMENT_TEXTURE(shadow_mask_tx_));
    for (int i = 0; i < 2; i++) {
      forward_fb_[i].ensure(GPU_ATTACHMENT_TEXTURE(depth_tx_),
                            GPU_ATTACHMENT_TEXTURE(accum_tx_[i]));
    }
    if (caps_.multiple_targets) {
      gbuffer_fb_.ensure(GPU_ATTACHMENT_TEXTURE(depth_tx_),
                         GPU_ATTACHMENT_TEXTURE(gb_diffuse_tx_),
                         GPU_ATTACHMENT_TEXTURE(gb_normal_tx_),
                         GPU_ATTACHMENT_TEXTURE(gb_depth_tx_),
                         GPU_ATTACHMENT_TEXTURE(gb_specular_tx_));
    }
    else {
      gpu::Texture *targets[4] = {gb_diffuse_tx_, gb_normal_tx_, gb_depth_tx_, gb_specular_tx_};
      for (int i = 0; i < 4; i++) {
        gbuffer_single_fb_[i].ensure(GPU_ATTACHMENT_TEXTURE(depth_tx_),
                                     GPU_ATTACHMENT_TEXTURE(targets[i]));
      }
    }
    tile_allocated_ = size;
  }

  void tile_buffers_free()
  {
    gb_diffuse_tx_.free();
    gb_normal_tx_.free();
    gb_depth_tx_.free();
    gb_specular_tx_.free();
    shadow_mask_tx_.free();
    depth_tx_.free();
    accum_tx_[0].free();
    accum_tx_[1].free();
    tile_mean_tx_.free();
    tile_allocated_ = int2(0);
  }

  /** False when some tile buffer writes at AGP memory speed. */
  bool tile_buffers_in_video_memory()
  {
    gpu::Texture *textures[] = {gb_diffuse_tx_,
                                gb_normal_tx_,
                                gb_depth_tx_,
                                gb_specular_tx_,
                                accum_tx_[0],
                                accum_tx_[1],
                                shadow_mask_tx_,
                                shadow_tx_,
                                tile_mean_tx_};
    constexpr int textures_len = 9;
    double rates[textures_len];
    double best = 0.0;
    for (int i = 0; i < textures_len; i++) {
      rates[i] = GPU_texture_write_rate(textures[i]);
      best = math::max(best, rates[i]);
    }
    if (best <= 0.0) {
      return true;
    }
    /* Much slower than the others (integrated GPUs only have system memory), and slow (fast
     * clears make some formats look many times faster on recent GPUs): on the Radeon 9600 video
     * memory writes at 4 GB/s and more, AGP memory at about 1. */
    for (int i = 0; i < textures_len; i++) {
      if (rates[i] < best * 0.4 && rates[i] < 2.5e9) {
        printf("MECHAZYIU: tile buffer %d writes at %.1f GB/s (system memory), "
               "shrinking the tiles\n",
               i,
               rates[i] * 1e-9);
        return false;
      }
    }
    return true;
  }

  /* ------------------------------------------------------------------ */
  /* Sampling. */

  /** Projection of a tile of the image, with a sub-pixel offset in pixels. */
  static float4x4 tile_winmat(const float4x4 &winmat, int2 size, int4 tile, float2 offset)
  {
    const float2 scale = float2(size) / float2(tile.z, tile.w);
    const float2 center = (float2(tile.x, tile.y) + float2(tile.z, tile.w) * 0.5f) /
                              float2(size) * 2.0f -
                          1.0f;
    const float2 shift = offset * 2.0f / float2(size);
    float4x4 t = float4x4::identity();
    t[0][0] = scale.x;
    t[1][1] = scale.y;
    t[3][0] = scale.x * (shift.x - center.x);
    t[3][1] = scale.y * (shift.y - center.y);
    return t * winmat;
  }

  /** Normalized device rectangle of the tile a light can reach, false if none. */
  static bool light_rect_get(const float3 &center, float radius, const float4x4 &winmat,
                             float4 &r_rect)
  {
    r_rect = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    const bool is_persp = winmat[3][3] == 0.0f;
    if (is_persp && -center.z - radius < 1e-3f) {
      /* Around or behind the camera. */
      return true;
    }
    float2 lo(FLT_MAX), hi(-FLT_MAX);
    for (int i = 0; i < 8; i++) {
      const float3 corner = center + float3((i & 1) ? radius : -radius,
                                            (i & 2) ? radius : -radius,
                                            (i & 4) ? radius : -radius);
      const float4 clip = winmat * float4(corner, 1.0f);
      if (clip.w <= 1e-6f) {
        return true;
      }
      const float2 ndc = float2(clip) / clip.w;
      lo = math::min(lo, ndc);
      hi = math::max(hi, ndc);
    }
    lo = math::max(lo, float2(-1.0f));
    hi = math::min(hi, float2(1.0f));
    if (lo.x >= hi.x || lo.y >= hi.y) {
      return false;
    }
    r_rect = float4(lo.x, lo.y, hi.x, hi.y);
    return true;
  }

  /**
   * Render one sample of a tile and mix it into the mean with the weight `mix`: the viewport's
   * mean (`mean_tx_`, with the depth in `view_depth_tx_`), or the tile's (final renders).
   */
  void render_sample(Manager &manager,
                     const float4x4 &camera_viewmat,
                     const float4x4 &camera_winmat,
                     int2 size,
                     int4 tile,
                     int sample,
                     float2 offset,
                     float mix,
                     bool viewport)
  {
    float4x4 viewmat = camera_viewmat, winmat = camera_winmat;
    dof_jitter(sample, viewmat, winmat);
    const float4x4 winmat_tile = tile_winmat(winmat, size, tile, offset);
    view_.sync(viewmat, winmat_tile);

    const bool is_persp = winmat_tile[3][3] == 0.0f;
    proj_type_ = float4(is_persp ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    proj_ = float4(1.0f / winmat_tile[0][0],
                   1.0f / winmat_tile[1][1],
                   is_persp ? winmat_tile[2][0] : -winmat_tile[3][0],
                   is_persp ? winmat_tile[2][1] : -winmat_tile[3][1]);

    /* The tile buffers are larger than edge tiles: only the tile's part is used, the screen
     * passes sample it with scaled texture coordinates. */
    const float2 used = float2(tile.z, tile.w) / float2(tile_allocated_);
    proj_type_ = float4(proj_type_.x, 0.0f, used.x, used.y);
    /* Depth of the cutout surfaces where their image covers the pixel. */
    GPU_framebuffer_bind(alpha_prepass_fb_);
    GPU_framebuffer_viewport_set(alpha_prepass_fb_, 0, 0, tile.z, tile.w);
    GPU_framebuffer_clear_depth_stencil(alpha_prepass_fb_, 1.0f, 0x00);
    manager.submit(alpha_prepass_ps_, view_);

    if (caps_.multiple_targets) {
      GPU_framebuffer_bind(gbuffer_fb_);
      GPU_framebuffer_viewport_set(gbuffer_fb_, 0, 0, tile.z, tile.w);
      const std::array<double4, 4> clear = {
          double4(0.0), double4(0.0), double4(0.0), double4(0.0)};
      GPU_framebuffer_multi_clear(gbuffer_fb_, clear);
      manager.submit(gbuffer_ps_, view_);
    }
    else {
      /* The surfaces once per target, the same depth each time. */
      for (int i = 0; i < 4; i++) {
        Framebuffer &fb = gbuffer_single_fb_[i];
        GPU_framebuffer_bind(fb);
        GPU_framebuffer_viewport_set(fb, 0, 0, tile.z, tile.w);
        fb.clear_color(float4(0.0f));
        gbuffer_target_ = float4(i == 0, i == 1, i == 2, i == 3);
        manager.submit(gbuffer_ps_, view_);
      }
    }

    /* Screen passes cover the used part of the buffers: their rectangles are in the tile's
     * normalized coordinates, mapped by the viewport. */
    auto bind = [&](Framebuffer &fb) {
      GPU_framebuffer_bind(fb);
      GPU_framebuffer_viewport_set(fb, 0, 0, tile.z, tile.w);
    };

    /* Ambient occlusion: the weights of its points into the buffer of the shadow masks (they come
     * later), then the occlusion and bounce light into the other half of the sample buffers (free
     * until the mix). */
    rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    ao_src_ = no_ao_tx_;
    if (ao_distance_ > 0.0f && !draft_) {
      ao_setup(sample);
      bind(shadow_mask_fb_);
      manager.submit(ao_ps_[proj_type_.x > 0.5f ? 0 : 1]);
      /* The mean of the previous samples, in the G-buffer's texture coordinates. */
      const float2 tile_size(tile_allocated_);
      if (viewport) {
        mean_src_ = mean_tx_;
        const float2 view_size(GPU_texture_width(mean_tx_), GPU_texture_height(mean_tx_));
        mean_uv_transform_ = float4(tile_size / view_size, float2(tile.x, tile.y) / view_size);
      }
      else {
        mean_src_ = tile_mean_tx_;
        mean_uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
      }
      bind(accum_fb_[1 - cur_]);
      manager.submit(bounce_ps_);
      ao_src_ = accum_tx_[1 - cur_];
    }

    /* World, emission, background. */
    rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    {
      /* Rows of the view to environment rotation: to world space (or, for studio lights
       * following the view, like EEVEE: x right, y toward the viewer, z up), then the
       * environment's rotation. */
      const float3x3 rotation = float3x3(viewmat);
      const float3 base[3] = {world_.view_space ? float3(1.0f, 0.0f, 0.0f) : rotation[0],
                              world_.view_space ? float3(0.0f, 0.0f, 1.0f) : rotation[1],
                              world_.view_space ? float3(0.0f, 1.0f, 0.0f) : rotation[2]};
      const float c = math::cos(world_.rotation_z), s = math::sin(world_.rotation_z);
      const float3 rows[3] = {base[0] * c + base[1] * s, base[1] * c - base[0] * s, base[2]};
      for (int i = 0; i < 3; i++) {
        world_rotation_[i] = float4(rows[i], 0.0f);
      }
      /* Spherical harmonics in view space: sh . (rows * d) = (sum of sh[i] * rows[i]) . d. */
      world_sh_[0] = float4(world_.sh0, 0.0f);
      for (int axis = 0; axis < 3; axis++) {
        float3 sh(0.0f);
        for (int j = 0; j < 3; j++) {
          sh += world_.sh1[j] * rows[j][axis];
        }
        world_sh_[axis + 1] = float4(sh, 0.0f);
      }
      /* The background like EEVEE's: blurred like reflections (EEVEE's roughness blur^2 looks
       * like roughness blur here), faded to transparent. */
      world_params_ = float4(environment_rgbm_range * world_.strength,
                             0.0f,
                             world_.background_blur,
                             transparent_ ? 0.0f : world_.background_opacity);
    }
    bind(accum_fb_[cur_]);
    manager.submit(base_ps_);

    /* Lights. */
    const float3x3 view_rotation = float3x3(viewmat);
    light_sample_positions_.resize(lights_.size());
    for (const int i : lights_.index_range()) {
      const LightData &light = lights_[i];
      /* Points of the light's shape, a different sequence for each light. */
      float2 rand(0.0f);
      if (sample > 0) {
        const float2 shift(math::fract(float(i) * 0.7548776f), math::fract(float(i) * 0.5698403f));
        rand = math::fract(float2(halton(sample, 5), halton(sample, 7)) + shift);
      }
      const float3 world_pos = (sample > 0 || light.type == LightType::SUN) ?
                                   light_sample_position(light, rand) :
                                   light.object_to_world.location();
      light_sample_positions_[i] = world_pos;
      float4 rect;
      if (light.type == LightType::SUN) {
        light_position_ = float4(math::normalize(view_rotation * world_pos), 0.0f);
        rect = float4(-1.0f, -1.0f, 1.0f, 1.0f);
      }
      else {
        const float3 view_pos = math::transform_point(viewmat, world_pos);
        if (!light_rect_get(view_pos, light.influence_radius, winmat_tile, rect)) {
          continue;
        }
        light_position_ = float4(view_pos, 1.0f / math::square(light.influence_radius));
      }
      const float3 forward = -math::normalize(view_rotation * light.object_to_world.z_axis());
      light_direction_ = float4(forward, 0.0f);
      light_color_ = float4(light.intensity * float(M_1_PI), light.specular_factor * 0.25f);
      const float smooth = math::max(light.spot_smooth, 1e-4f);
      light_spot_ = float4(
          1.0f / smooth, -light.spot_cos_half / smooth, light.diffuse_factor, 0.0f);
      light_mask_src_ = white_tx_;
      if (light.shadow && !draft_ && shadow_render(manager, light, world_pos, viewmat, sample)) {
        rect_ = rect;
        bind(shadow_mask_fb_);
        proj_type_ = float4(proj_type_.x, 0.0f, used.x, used.y);
        const int kind = light.type == LightType::SUN ? 2 : light.type == LightType::SPOT ? 1 : 0;
        manager.submit(shadow_mask_ps_[kind]);
        light_mask_src_ = shadow_mask_tx_;
      }
      rect_ = rect;

      if (caps_.float_blending) {
        bind(accum_fb_[cur_]);
        manager.submit(light_ps_[int(light.type)]);
        continue;
      }
      accum_src_ = accum_tx_[cur_];
      bind(accum_fb_[1 - cur_]);
      manager.submit(light_ps_[int(light.type)]);
      copy_src_ = accum_tx_[1 - cur_];
      uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
      bind(accum_fb_[cur_]);
      manager.submit(copy_ps_);
    }

    blended_render(manager, viewmat, winmat_tile, tile);

    /* Mix the sample into the mean (in the other buffer), then keep it. */
    rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    copy_src_ = accum_tx_[cur_];
    mix_weight_ = float4(mix);
    const float2 tile_size(tile_allocated_);
    if (viewport) {
      mean_src_ = mean_tx_;
      const float2 view_size(GPU_texture_width(mean_tx_), GPU_texture_height(mean_tx_));
      uv_transform_ = float4(tile_size / view_size, float2(tile.x, tile.y) / view_size);
    }
    else {
      mean_src_ = tile_mean_tx_;
      uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
    }
    bind(accum_fb_[1 - cur_]);
    manager.submit(mix_ps_);

    copy_src_ = accum_tx_[1 - cur_];
    if (!viewport) {
      uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
      bind(tile_mean_fb_);
      manager.submit(copy_ps_);
      return;
    }
    /* The tile's part of the view, and its depth for the overlays (drafts stretched over it). */
    proj_type_ = float4(proj_type_.x, 0.0f, 1.0f, 1.0f);
    const float2 view_size(GPU_texture_width(mean_tx_), GPU_texture_height(mean_tx_));
    const float scale = float(draft_scale_);
    rect_ = float4(float2(tile.x, tile.y) * scale / view_size * 2.0f - 1.0f,
                   float2(tile.x + tile.z, tile.y + tile.w) * scale / view_size * 2.0f - 1.0f);
    uv_transform_ = float4(view_size / (tile_size * scale), -float2(tile.x, tile.y) / tile_size);
    GPU_framebuffer_bind(mean_fb_);
    manager.submit(copy_ps_);
    copy_src_ = gb_depth_tx_;
    GPU_framebuffer_bind(view_depth_fb_);
    manager.submit(copy_ps_);
  }

  /** Normalized device rectangle of world space bounds, false when off the tile. */
  static bool bounds_rect_get(const float3 &bounds_min,
                              const float3 &bounds_max,
                              const float4x4 &viewmat,
                              const float4x4 &winmat,
                              float4 &r_rect)
  {
    r_rect = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    float2 lo(FLT_MAX), hi(-FLT_MAX);
    for (int i = 0; i < 8; i++) {
      const float3 corner((i & 1) ? bounds_max.x : bounds_min.x,
                          (i & 2) ? bounds_max.y : bounds_min.y,
                          (i & 4) ? bounds_max.z : bounds_min.z);
      const float4 clip = winmat * (viewmat * float4(corner, 1.0f));
      if (clip.w <= 1e-6f) {
        /* Around or behind the camera. */
        return true;
      }
      lo = math::min(lo, float2(clip) / clip.w);
      hi = math::max(hi, float2(clip) / clip.w);
    }
    lo = math::max(lo, float2(-1.0f));
    hi = math::min(hi, float2(1.0f));
    if (lo.x >= hi.x || lo.y >= hi.y) {
      return false;
    }
    r_rect = float4(lo.x, lo.y, hi.x, hi.y);
    return true;
  }

  /**
   * Blended surfaces, back to front: each pass draws over the sample. Without floating point
   * blending the pass reads the sample and writes the other buffer (first given the sample's
   * rectangle), which then holds it.
   */
  void blended_render(Manager &manager,
                      const float4x4 &viewmat,
                      const float4x4 &winmat_tile,
                      int4 tile)
  {
    if (blended_.is_empty()) {
      return;
    }
    const float3x3 view_rotation = float3x3(viewmat);
    Vector<BlendedDraw *> order;
    for (std::unique_ptr<BlendedDraw> &draw : blended_) {
      draw->depth = math::transform_point(viewmat,
                                          (draw->bounds_min + draw->bounds_max) * 0.5f).z;
      order.append(draw.get());
    }
    std::sort(order.begin(), order.end(), [](const BlendedDraw *a, const BlendedDraw *b) {
      return a->depth < b->depth;
    });

    auto pass = [&](PassMain &ps, const float4 &rect) {
      Framebuffer &target = forward_fb_[caps_.float_blending ? cur_ : 1 - cur_];
      if (!caps_.float_blending) {
        rect_ = rect;
        copy_src_ = accum_tx_[cur_];
        uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
        GPU_framebuffer_bind(accum_fb_[1 - cur_]);
        GPU_framebuffer_viewport_set(accum_fb_[1 - cur_], 0, 0, tile.z, tile.w);
        manager.submit(copy_ps_);
        forward_src_ = accum_tx_[cur_];
      }
      GPU_framebuffer_bind(target);
      GPU_framebuffer_viewport_set(target, 0, 0, tile.z, tile.w);
      manager.submit(ps, view_);
      if (!caps_.float_blending) {
        cur_ = 1 - cur_;
      }
    };

    for (BlendedDraw *draw : order) {
      float4 rect;
      if (!bounds_rect_get(draw->bounds_min, draw->bounds_max, viewmat, winmat_tile, rect)) {
        continue;
      }
      for (int side = draw->double_sided ? 1 : 0; side >= 0; side--) {
        pass(draw->base_ps[side], rect);
        for (const int i : lights_.index_range()) {
          const LightData &light = lights_[i];
          const float3 &pos = light_sample_positions_[i];
          if (light.type != LightType::SUN) {
            const float3 nearest = math::clamp(pos, draw->bounds_min, draw->bounds_max);
            if (math::distance(nearest, pos) > light.influence_radius) {
              continue;
            }
          }
          const bool sun = light.type == LightType::SUN;
          forward_light_position_ = sun ?
                                        float4(math::normalize(view_rotation * pos), 0.0f) :
                                        float4(math::transform_point(viewmat, pos), 1.0f);
          const float3 forward = -math::normalize(view_rotation *
                                                  light.object_to_world.z_axis());
          forward_light_direction_ = float4(
              forward, sun ? 0.0f : 1.0f / math::square(light.influence_radius));
          forward_light_color_ = float4(light.intensity * float(M_1_PI),
                                        light.specular_factor * 0.25f);
          const float smooth = math::max(light.spot_smooth, 1e-4f);
          forward_light_spot_ = light.type == LightType::SPOT ?
                                    float4(1.0f / smooth,
                                           -light.spot_cos_half / smooth,
                                           1.0f,
                                           light.diffuse_factor) :
                                light.type == LightType::AREA ?
                                    float4(1.0f, 0.0f, 0.0f, light.diffuse_factor) :
                                    float4(0.0f, 1.0f, 0.0f, light.diffuse_factor);
          pass(draw->light_ps[side], rect);
        }
      }
    }
  }

  /**
   * Render the shadow map of a light at a point of its shape, and set the parameters of its
   * shadow mask pass. False when nothing casts shadows.
   */
  bool shadow_render(Manager &manager,
                     const LightData &light,
                     const float3 &world_pos,
                     const float4x4 &viewmat,
                     int sample)
  {
    if (casters_min_.x > casters_max_.x) {
      return false;
    }
    const float4x4 viewinv = math::invert(viewmat);
    GPU_framebuffer_bind(shadow_fb_);
    /* Distances are normalized: 1 is nothing. */
    GPU_framebuffer_clear_color_depth(shadow_fb_, double4(1.0), 1.0f);

    /* Each sample looks up the map somewhere else in a texel. */
    const float2 jitter = sample > 0 ?
                              float2(halton(sample, 11), halton(sample, 13)) - 0.5f :
                              float2(0.0f);
    const float2 atlas_texel = 1.0f / float2(shadow_atlas_width, shadow_atlas_height);
    shadow_params_ = float4(jitter * atlas_texel * 2.0f, 0.0f, 0.0f);
    translucent_depth_src_ = white_tx_;
    transmittance_src_ = white_tx_;
    if (has_translucent_casters_) {
      GPU_framebuffer_bind(translucent_fb_);
      GPU_framebuffer_clear_color_depth(translucent_fb_, double4(1.0), 1.0f);
      GPU_framebuffer_bind(transmittance_fb_);
      GPU_framebuffer_clear_color(transmittance_fb_, double4(1.0));
      translucent_depth_src_ = translucent_depth_tx_;
      transmittance_src_ = transmittance_tx_;
    }
    /* A face of the shadow maps: the opaque casters, then the transparent ones (their maps have
     * half the resolution). */
    auto face_render = [&](int ortho, int x, int y, int size) {
      GPU_framebuffer_bind(shadow_fb_);
      GPU_framebuffer_viewport_set(shadow_fb_, x, y, size, size);
      manager.submit(shadow_ps_[ortho], shadow_view_);
      if (!has_translucent_casters_) {
        return;
      }
      GPU_framebuffer_bind(translucent_fb_);
      GPU_framebuffer_viewport_set(translucent_fb_, x / 2, y / 2, size / 2, size / 2);
      manager.submit(translucent_ps_[ortho], shadow_view_);
      GPU_framebuffer_bind(transmittance_fb_);
      GPU_framebuffer_viewport_set(transmittance_fb_, x / 2, y / 2, size / 2, size / 2);
      manager.submit(transmit_ps_, shadow_view_);
    };

    auto face_view = [](const float3 &right, const float3 &up, const float3 &forward,
                        const float3 &origin) {
      float4x4 m = float4x4::identity();
      const float3 back = -forward;
      for (int i = 0; i < 3; i++) {
        m[i][0] = right[i];
        m[i][1] = up[i];
        m[i][2] = back[i];
      }
      m[3][0] = -math::dot(right, origin);
      m[3][1] = -math::dot(up, origin);
      m[3][2] = -math::dot(back, origin);
      return m;
    };

    if (light.type == LightType::SUN) {
      /* Orthographic, over the shadow casters. */
      const float3 forward = -math::normalize(world_pos);
      const float3 hint = math::abs(forward.z) < 0.9f ? float3(0.0f, 0.0f, 1.0f) :
                                                        float3(1.0f, 0.0f, 0.0f);
      const float3 right = math::normalize(math::cross(forward, hint));
      const float3 up = math::cross(right, forward);
      const float4x4 view = face_view(right, up, forward, float3(0.0f));
      float3 lo(FLT_MAX), hi(-FLT_MAX);
      for (int corner = 0; corner < 8; corner++) {
        const float3 p((corner & 1) ? casters_max_.x : casters_min_.x,
                       (corner & 2) ? casters_max_.y : casters_min_.y,
                       (corner & 4) ? casters_max_.z : casters_min_.z);
        const float3 v = math::transform_point(view, p);
        lo = math::min(lo, v);
        hi = math::max(hi, v);
      }
      const float margin = math::reduce_max(hi - lo) * 0.01f + 1e-3f;
      lo -= margin;
      hi += margin;
      const float4x4 win = math::projection::orthographic(lo.x, hi.x, lo.y, hi.y, -hi.z, -lo.z);
      shadow_view_.sync(view, win);
      face_render(1, 0, 0, shadow_atlas_height);

      const float4x4 view_to_clip = win * view * viewinv;
      for (int i = 0; i < 4; i++) {
        shadow_mat_[i] = view_to_clip[i];
      }
      const float texel = math::max(hi.x - lo.x, hi.y - lo.y) / float(shadow_atlas_height);
      shadow_params_.w = texel * 1.5f;
      shadow_bias_ = float4(1.0f, 2.0f * texel / (hi.z - lo.z), 0.0f, 0.0f);
      return true;
    }

    const float far = light.influence_radius;
    const float near = math::max(far * 1e-4f, 1e-3f);
    shadow_light_ = float4(world_pos, 1.0f / far);
    shadow_light_view_ = float4(math::transform_point(viewmat, world_pos), 1.0f / far);

    if (light.type == LightType::SPOT) {
      const float3 forward = -math::normalize(light.object_to_world.z_axis());
      const float3 right = math::normalize(light.object_to_world.x_axis());
      const float3 up = math::normalize(math::cross(right, forward));
      const float4x4 view = face_view(math::cross(forward, up), up, forward, world_pos);
      const float half_angle = math::min(math::acos(light.spot_cos_half) + 0.05f, 1.48f);
      const float extent = math::tan(half_angle) * near;
      const float4x4 win = math::projection::perspective(-extent, extent, -extent, extent, near, far);
      shadow_view_.sync(view, win);
      face_render(0, 0, 0, shadow_atlas_height);

      const float4x4 view_to_clip = win * view * viewinv;
      for (int i = 0; i < 4; i++) {
        shadow_mat_[i] = view_to_clip[i];
      }
      shadow_params_.z = 1.5f * 2.0f * math::tan(half_angle) / float(shadow_atlas_height);
      shadow_bias_ = float4(1.0f - 0.002f, 0.0f, 0.0f, 0.0f);
      return true;
    }

    /* Point and area lights: 6 faces of 90 degrees, aligned with the view axes (the mask pass
     * works in view space). */
    const float3 axes[3] = {math::normalize(viewinv.x_axis()),
                            math::normalize(viewinv.y_axis()),
                            math::normalize(viewinv.z_axis())};
    const float4x4 win = math::projection::perspective(-near, near, -near, near, near, far);
    for (int face = 0; face < 6; face++) {
      const int axis = face / 2;
      const float3 forward = (face & 1) ? -axes[axis] : axes[axis];
      /* The two other axes in order: the mask pass projects on them. */
      const float3 &right = axes[axis == 0 ? 1 : 0];
      const float3 &up = axes[axis == 2 ? 1 : 2];
      shadow_view_.sync(face_view(right, up, forward, world_pos), win);
      face_render(0, (face % 4) * shadow_cube_size, (face / 4) * shadow_cube_size, shadow_cube_size);
    }
    shadow_params_.z = 2.5f * 2.0f / float(shadow_cube_size);
    shadow_bias_ = float4(1.0f - 0.005f, 0.0f, 0.0f, 0.0f);
    return true;
  }

  /** Pixel offset of a sample, all samples weigh the same (`r_weight`, 1). */
  float2 sample_offset(int sample, float &r_weight) const
  {
    r_weight = 1.0f;
    if (sample == 0) {
      return float2(0.0f);
    }
    const float2 offset(blackman_harris_sample(halton(sample, 2)),
                        blackman_harris_sample(halton(sample, 3)));
    return offset * math::max(filter_size_, 0.01f);
  }

  /* ------------------------------------------------------------------ */
  /* Viewport. */

  /**
   * The bloom of the viewport's mean (EEVEE Legacy's): the bright parts at half the resolution,
   * halved again and again, added back up. The output adds the last level (`bloom_out_`).
   */
  void bloom_render(Manager &manager, int2 size)
  {
    bloom_out_ = nullptr;
    if (!bloom_.enabled || bloom_.intensity <= 0.0f || size.x < 4 || size.y < 4) {
      return;
    }
    const BloomLevels levels = bloom_levels_get(size, bloom_.radius);
    const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
    rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    proj_type_ = float4(0.0f, 0.0f, 1.0f, 1.0f);
    auto pass = [&](Texture &target, Framebuffer &fb, int2 target_size, PassSimple &ps) {
      target.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, target_size, usage);
      fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(target));
      GPU_framebuffer_bind(fb);
      GPU_framebuffer_viewport_set(fb, 0, 0, target_size.x, target_size.y);
      manager.submit(ps);
    };
    auto texel = [&](int2 source_size) {
      return float4(1.0f / float2(source_size), levels.sample_scale, 0.0f);
    };

    int2 level_size = math::max(size / 2, int2(1));
    bloom_curve_ = bloom_.curve();
    bloom_params_ = float4(bloom_.clamp, 0.0f, 0.0f, 0.0f);
    bloom_src_ = mean_tx_;
    bloom_texel_ = texel(size);
    pass(bloom_bright_tx_, bloom_bright_fb_, level_size, bloom_prefilter_ps_);

    int2 sizes[bloom_levels_max];
    bloom_src_ = bloom_bright_tx_;
    for (int i = 0; i < levels.len; i++) {
      bloom_texel_ = texel(level_size);
      level_size = math::max(level_size / 2, int2(1));
      sizes[i] = level_size;
      pass(bloom_down_tx_[i], bloom_down_fb_[i], level_size, bloom_downsample_ps_);
      bloom_src_ = bloom_down_tx_[i];
    }
    int2 last_size = sizes[levels.len - 1];
    for (int i = levels.len - 2; i >= 0; i--) {
      bloom_base_ = bloom_down_tx_[i];
      bloom_texel_ = texel(last_size);
      pass(bloom_up_tx_[i], bloom_up_fb_[i], sizes[i], bloom_upsample_ps_);
      bloom_src_ = bloom_up_tx_[i];
      last_size = sizes[i];
    }
    /* Once more, to half the resolution (the output samples it once). */
    bloom_base_ = black_tx_;
    bloom_texel_ = texel(last_size);
    pass(bloom_bright_tx_, bloom_bright_fb_, math::max(size / 2, int2(1)), bloom_upsample_ps_);
    bloom_out_ = bloom_bright_tx_;
    bloom_color_ = float4(bloom_.color * bloom_.intensity, 0.0f);
  }

  void draw(Manager &manager) final
  {
    DefaultTextureList *dtxl = draw_ctx->viewport_texture_list_get();
    if (!caps_.supported) {
      STRNCPY(info, unsupported_message);
      return;
    }
    const int2 size(GPU_texture_width(dtxl->color), GPU_texture_height(dtxl->color));
    const View &main_view = View::default_get();
    const float4x4 viewmat = main_view.viewmat();
    const float4x4 winmat = main_view.winmat();

    /* Start over when anything changed. */
    const uint64_t update = DEG_get_update_count(draw_ctx->depsgraph);
    const float4x4 persmat = winmat * viewmat;
    uint64_t settings = uint64_t(use_scene_lights_) | uint64_t(use_scene_world_) << 1;
    if (draw_ctx->v3d) {
      const View3DShading &shading = draw_ctx->v3d->shading;
      const float values[4] = {shading.studiolight_rot_z,
                               shading.studiolight_intensity,
                               shading.studiolight_background,
                               shading.studiolight_blur};
      settings ^= std::hash<std::string>{}(shading.lookdev_light) ^ uint64_t(shading.type) << 2 ^
                  uint64_t(shading.flag & V3D_SHADING_STUDIOLIGHT_VIEW_ROTATION) << 3;
      for (int i = 0; i < 4; i++) {
        uint32_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        settings ^= uint64_t(bits) << (8 + i * 13);
      }
    }
    if (update != last_update_ || persmat != last_persmat_ || size != last_size_ ||
        settings != last_settings_)
    {
      sample_ = 0;
      last_update_ = update;
      last_persmat_ = persmat;
      last_size_ = size;
      last_settings_ = settings;
    }

    DRW_submission_start();
    tiles_ensure(size, true);

    /* Viewport image renders do every sample at once. */
    const bool image_render = draw_ctx->is_viewport_image_render();
    const int samples_len = image_render ? math::max(1, scene_->eevee.taa_render_samples) :
                                           samples_len_;
    int samples_todo = image_render ? samples_len : 1;
    if (image_render) {
      sample_ = 0;
    }
    while (samples_todo-- > 0 && (samples_len <= 0 || sample_ < samples_len)) {
      draft_ = !image_render && samples_len != 1 && sample_ == 0;
      if (sample_ == 0) {
        weight_sum_ = 0.0f;
        mean_tx_.clear(float4(0.0f));
        view_depth_tx_.clear(float4(0.0f));
      }
      else if (sample_ == 1 && !image_render && samples_len != 1) {
        /* The first full sample replaces the draft. */
        weight_sum_ = 0.0f;
      }
      float weight;
      const float2 offset = sample_offset(sample_, weight);
      weight_sum_ += weight;
      /* Drafts at half the resolution, when the view is a single tile. */
      const int2 draft_size = (size + 1) / 2;
      draft_scale_ = (draft_ && plan_.len() == 1) ? 2 : 1;
      if (draft_scale_ == 2) {
        render_sample(manager,
                      viewmat,
                      winmat,
                      draft_size,
                      int4(0, 0, draft_size.x, draft_size.y),
                      sample_,
                      offset,
                      1.0f,
                      true);
      }
      else {
        for (const int t : IndexRange(plan_.len())) {
          const int4 tile = plan_.rect(t);
          render_sample(
              manager, viewmat, winmat, size, tile, sample_, offset, weight / weight_sum_, true);
        }
      }
      draft_scale_ = 1;
      sample_++;
    }

    bloom_render(manager, size);

    /* Output: the mean, and the depth of the (not jittered) view for the overlays. */
    output_fb_.ensure(GPU_ATTACHMENT_TEXTURE(dtxl->depth), GPU_ATTACHMENT_TEXTURE(dtxl->color));
    GPU_framebuffer_bind(output_fb_);
    rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    proj_type_ = float4(0.0f, 0.0f, 1.0f, 1.0f);
    uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
    const bool is_persp = winmat[3][3] == 0.0f;
    depth_proj_ = float4(winmat[2][2], winmat[3][2], is_persp ? 1.0f : 0.0f, 0.0f);
    manager.submit(output_ps_[bloom_out_ != nullptr]);
    DRW_submission_end();

    if (samples_len <= 0 || sample_ < samples_len) {
      DRW_viewport_request_redraw();
    }
    if (samples_len > 0) {
      SNPRINTF(info, "Sample %d / %d", math::min(sample_, samples_len), samples_len);
    }
    else {
      SNPRINTF(info, "Sample %d", sample_);
    }
    if (plan_.len() > 1) {
      const size_t len = strlen(info);
      BLI_snprintf(info + len, sizeof(info) - len, ", %d tiles", plan_.len());
    }
  }

  /* ------------------------------------------------------------------ */
  /* Final render. */

  void render_image(Manager &manager,
                    RenderEngine *engine,
                    RenderLayer *layer,
                    const char *viewname,
                    const float4x4 &viewmat,
                    const float4x4 &winmat,
                    int2 size)
  {
    if (!caps_.supported) {
      RE_engine_report(engine, RPT_ERROR, unsupported_message);
      return;
    }
    tiles_ensure(size, false);

    RenderPass *combined = RE_pass_find_by_name(layer, RE_PASSNAME_COMBINED, viewname);
    RenderPass *depth = RE_pass_find_by_name(layer, RE_PASSNAME_DEPTH, viewname);
    const int tiles_len = plan_.len();
    for (const int t : IndexRange(tiles_len)) {
      const int4 tile = plan_.rect(t);
      tile_mean_tx_.clear(float4(0.0f));
      float weight_sum = 0.0f;
      for (const int s : IndexRange(samples_len_)) {
        if (RE_engine_test_break(engine)) {
          return;
        }
        float weight;
        const float2 offset = sample_offset(s, weight);
        weight_sum += weight;
        render_sample(manager, viewmat, winmat, size, tile, s, offset, weight / weight_sum, false);
        GPU_render_step();

        char stats[128];
        SNPRINTF(stats, "Tile %d / %d, sample %d / %d", t + 1, tiles_len, s + 1, samples_len_);
        RE_engine_update_stats(engine, nullptr, stats);
        RE_engine_update_progress(engine, (float(t) + float(s + 1) / samples_len_) / tiles_len);
      }
      if (getenv("MECHAZYIU_DEBUG")) {
        debug_print(tile);
      }
      if (combined) {
        tile_read(tile_mean_tx_, tile, 4, combined->ibuf->float_data_for_write(), size, false);
      }
      if (depth) {
        tile_read(gb_depth_tx_, tile, 1, depth->ibuf->float_data_for_write(), size, true);
      }
    }
    if (combined && bloom_.enabled && bloom_.intensity > 0.0f) {
      RE_engine_update_stats(engine, nullptr, "Bloom");
      bloom_apply(MutableSpan<float4>(
                      reinterpret_cast<float4 *>(combined->ibuf->float_data_for_write()),
                      int64_t(size.x) * size.y),
                  size,
                  bloom_);
    }
  }

  /** `MECHAZYIU_DEBUG`: what the passes got, for a final render tile. */
  void debug_print(int4 tile)
  {
    printf("MECHAZYIU debug: world %f %f %f, background %f %f %f, %d lights, %d materials\n",
           world_.sh0.x,
           world_.sh0.y,
           world_.sh0.z,
           world_.background.x,
           world_.background.y,
           world_.background.z,
           int(lights_.size()),
           int(materials_.size()));
    gpu::Texture *textures[] = {
        gb_diffuse_tx_, gb_normal_tx_, gb_depth_tx_, gb_specular_tx_, tile_mean_tx_};
    const char *names[] = {"diffuse", "normal", "depth", "specular", "accum"};
    for (int i = 0; i < 5; i++) {
      float *data = static_cast<float *>(GPU_texture_read(textures[i], GPU_DATA_FLOAT, 0));
      if (data == nullptr) {
        continue;
      }
      const int channels = i == 2 ? 1 : 4;
      const int width = GPU_texture_width(textures[i]);
      const int2 points[2] = {int2(2, 2), int2(tile.z / 2, tile.w / 2)};
      for (const int2 p : points) {
        const float *v = data + (size_t(p.y) * width + p.x) * channels;
        printf("  %s at %d,%d: %f %f %f %f\n",
               names[i],
               p.x,
               p.y,
               v[0],
               channels > 1 ? v[1] : 0.0f,
               channels > 2 ? v[2] : 0.0f,
               channels > 3 ? v[3] : 0.0f);
      }
      MEM_delete(data);
    }
    fflush(stdout);
  }

  /** Copy a tile into an image buffer of the full size. */
  void tile_read(gpu::Texture *texture,
                 int4 tile,
                 int channels,
                 float *dst,
                 int2 size,
                 bool is_depth)
  {
    float *data = static_cast<float *>(GPU_texture_read(texture, GPU_DATA_FLOAT, 0));
    if (data == nullptr || dst == nullptr) {
      MEM_delete(data);
      return;
    }
    const int src_width = GPU_texture_width(texture);
    for (int y = 0; y < tile.w; y++) {
      const float *src_row = data + size_t(y) * src_width * channels;
      float *dst_row = dst + (size_t(tile.y + y) * size.x + tile.x) * channels;
      if (is_depth) {
        for (int x = 0; x < tile.z; x++) {
          /* Background: no depth. */
          dst_row[x] = src_row[x] > 0.0f ? src_row[x] : 1e10f;
        }
      }
      else {
        memcpy(dst_row, src_row, sizeof(float) * channels * tile.z);
      }
    }
    MEM_delete(data);
  }
};

DrawEngine *Engine::create_instance()
{
  return new Instance();
}

void Engine::free_static()
{
  ShaderCache::release();
}

}  // namespace mechazyiu

/* -------------------------------------------------------------------- */
/** \name Render Engine
 * \{ */

static void mechazyiu_render_to_image(RenderEngine *engine, RenderLayer *layer, const rcti /*rect*/)
{
  using namespace blender::draw;
  const DRWContext *draw_ctx = DRW_context_get();
  Depsgraph *depsgraph = draw_ctx->depsgraph;
  Object *camera_ob = DEG_get_evaluated(depsgraph, RE_GetCamera(engine->re));

  float4x4 winmat, viewinv;
  RE_GetCameraWindow(engine->re, camera_ob, winmat.ptr());
  RE_GetCameraModelMatrix(engine->re, camera_ob, viewinv.ptr());
  const float4x4 viewmat = math::invert(viewinv);

  DRW_cache_restart();
  View::default_set(viewmat, winmat);

  mechazyiu::Instance instance;
  instance.init_scene();

  Manager &manager = *DRW_manager_get();
  manager.begin_sync();
  instance.begin_sync();
  DRW_render_object_iter(
      engine, depsgraph, [&](ObjectRef &ob_ref, RenderEngine * /*engine*/, Depsgraph * /*dg*/) {
        instance.object_sync(ob_ref, manager);
      });
  instance.end_sync();
  manager.end_sync();

  DRW_submission_start();
  const float2 size = draw_ctx->viewport_size_get();
  instance.render_image(manager,
                        engine,
                        layer,
                        RE_GetActiveRenderView(engine->re),
                        viewmat,
                        winmat,
                        int2(size));
  DRW_submission_end();
}

static void mechazyiu_render_update_passes(RenderEngine *engine, Scene *scene, ViewLayer *view_layer)
{
  if (view_layer->passflag & SCE_PASS_COMBINED) {
    RE_engine_register_pass(engine, scene, view_layer, RE_PASSNAME_COMBINED, 4, "RGBA", SOCK_RGBA);
  }
  if (view_layer->passflag & SCE_PASS_DEPTH) {
    RE_engine_register_pass(engine, scene, view_layer, RE_PASSNAME_DEPTH, 1, "Z", SOCK_FLOAT);
  }
}

static void mechazyiu_render(RenderEngine *engine, Depsgraph *depsgraph)
{
  DRW_render_to_image(engine, depsgraph, mechazyiu_render_to_image, [](RenderResult *) {});
}

RenderEngineType DRW_engine_viewport_mechazyiu_type = {
    /*next*/ nullptr,
    /*prev*/ nullptr,
    /*idname*/ "BLENDER_MECHAZYIU",
    /*name*/ N_("MECHAZYIU"),
    /*flag*/ RE_INTERNAL | RE_USE_PREVIEW | RE_USE_GPU_CONTEXT,
    /*update*/ nullptr,
    /*render*/ &mechazyiu_render,
    /*render_frame_finish*/ nullptr,
    /*draw*/ nullptr,
    /*bake*/ nullptr,
    /*view_update*/ nullptr,
    /*view_draw*/ nullptr,
    /*update_script_node*/ nullptr,
    /*update_render_passes*/ &mechazyiu_render_update_passes,
    /*update_custom_camera*/ nullptr,
    /*draw_engine*/ nullptr,
    /*rna_ext*/
    {
        /*data*/ nullptr,
        /*srna*/ nullptr,
        /*call*/ nullptr,
    },
};

/** \} */

}  // namespace blender
