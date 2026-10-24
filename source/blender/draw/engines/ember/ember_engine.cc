/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * Ember render engine: see `ember_private.hh`.
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
#include "BKE_editmesh.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph_query.hh"

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

#include "ember_engine.h" /* Own include. */
#include "ember_private.hh"

namespace blender {

namespace ember {

using namespace draw;

/** Materials of a view (the index is in the G-buffer, 8 bits). */
constexpr int material_table_len = 256;

/** Video memory per pixel of a tile: 4 G-buffer targets, depth, 2 half float accumulations. */
constexpr int64_t tile_bytes_per_pixel = 4 * 4 + 4 + 2 * 8;
/** Running mean (half float) and linear depth of the whole viewport. */
constexpr int64_t view_bytes_per_pixel = 8 + 4;
/** Render targets are at most 2048 pixels wide on R300 hardware. */
constexpr int max_tile_size = 2048;

/* -------------------------------------------------------------------- */
/** \name Shaders
 * \{ */

class ShaderCache {
 public:
  gpu::StaticShader gbuffer[2 /*texture*/][2 /*flip*/] = {
      {{"ember_gbuffer"}, {"ember_gbuffer_flip"}},
      {{"ember_gbuffer_texture"}, {"ember_gbuffer_texture_flip"}}};
  gpu::StaticShader base = {"ember_base"};
  gpu::StaticShader light[light_type_len] = {
      {"ember_light_point"}, {"ember_light_spot"}, {"ember_light_sun"}, {"ember_light_area"}};
  gpu::StaticShader copy = {"ember_copy"};
  gpu::StaticShader output = {"ember_output"};

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

/** Fewest tiles of at most `max_pixels` pixels covering the image, as square as possible. */
static TilePlan tile_plan_get(int2 size, int64_t max_pixels)
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

class Instance : public DrawEngine {
 public:
  const DRWContext *draw_ctx = nullptr;

 private:
  View view_ = {"ember_view"};

  /* Scene. */
  const Scene *scene_ = nullptr;
  bool use_scene_lights_ = true;
  bool use_scene_world_ = true;
  bool is_render_ = false;
  int samples_len_ = 16;
  float filter_size_ = 1.5f;
  float light_threshold_ = 0.01f;
  bool transparent_ = false;
  WorldLighting world_;
  Vector<LightData> lights_;
  Vector<MaterialData> materials_;
  Map<const Material *, int> material_indices_;
  /** G-buffer sub-passes of each material: [index][textured][back faces]. */
  Map<int3, PassMain::Sub *> material_passes_;
  Set<gpu::Texture *> textures_;
  int64_t texture_bytes_ = 0;
  Vector<GPUMaterial *> dummy_gpu_materials_;

  /* Passes. */
  PassMain gbuffer_ps_ = {"ember.gbuffer"};
  PassSimple base_ps_ = {"ember.base"};
  PassSimple light_ps_[light_type_len] = {
      {"ember.light_point"}, {"ember.light_spot"}, {"ember.light_sun"}, {"ember.light_area"}};
  PassSimple copy_ps_ = {"ember.copy"};
  PassSimple output_ps_ = {"ember.output"};

  /* Tile buffers. */
  Texture gb_diffuse_tx_ = {"ember.gb_diffuse"};
  Texture gb_normal_tx_ = {"ember.gb_normal"};
  Texture gb_depth_tx_ = {"ember.gb_depth"};
  Texture gb_specular_tx_ = {"ember.gb_specular"};
  Texture depth_tx_ = {"ember.depth"};
  Texture accum_tx_[2] = {{"ember.accum0"}, {"ember.accum1"}};
  Framebuffer gbuffer_fb_ = {"ember.gbuffer"};
  Framebuffer accum_fb_[2] = {{"ember.accum0"}, {"ember.accum1"}};
  TilePlan plan_;
  int2 tile_allocated_ = int2(0);
  int64_t budget_ = 0;
  /** Accumulation buffer holding the mean. */
  int cur_ = 0;

  /* Viewport: mean and linear depth of the whole view. */
  Texture mean_tx_ = {"ember.mean"};
  Texture view_depth_tx_ = {"ember.view_depth"};
  Framebuffer mean_fb_ = {"ember.mean"};
  Framebuffer view_depth_fb_ = {"ember.view_depth"};
  Framebuffer output_fb_ = {"ember.output"};

  Texture material_tx_ = {"ember.materials"};

  /* Values read by the passes when they are submitted. */
  float4 rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
  float4 proj_ = float4(0.0f);
  float4 proj_type_ = float4(0.0f);
  float4 world_sh_[4] = {};
  float4 background_ = float4(0.0f);
  float4 accum_uv_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
  float4 light_position_ = float4(0.0f);
  float4 light_direction_ = float4(0.0f);
  float4 light_color_ = float4(0.0f);
  float4 light_spot_ = float4(0.0f);
  float4 uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
  float4 depth_proj_ = float4(0.0f);
  gpu::Texture *accum_src_ = nullptr;
  gpu::Texture *copy_src_ = nullptr;

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
    return "Ember";
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
                                world_lighting_studio_get(*v3d);
  }

  void begin_sync() final
  {
    lights_.clear();
    materials_.clear();
    material_indices_.clear();
    material_passes_.clear();
    textures_.clear();
    texture_bytes_ = 0;

    gbuffer_ps_.init();
    gbuffer_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH |
                          DRW_STATE_DEPTH_LESS_EQUAL);

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
      ps.bind_texture("accum_tx", &accum_src_);
    };

    screen_pass(base_ps_, shaders.base.get());
    gbuffer_read(base_ps_);
    base_ps_.bind_texture("material_tx", &material_tx_);
    base_ps_.push_constant("world_sh", world_sh_, 4);
    base_ps_.push_constant("background", &background_);
    base_ps_.push_constant("accum_uv", &accum_uv_);
    base_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);

    for (int type = 0; type < light_type_len; type++) {
      PassSimple &ps = light_ps_[type];
      screen_pass(ps, shaders.light[type].get());
      gbuffer_read(ps);
      ps.push_constant("light_position", &light_position_);
      ps.push_constant("light_direction", &light_direction_);
      ps.push_constant("light_color", &light_color_);
      ps.push_constant("light_spot", &light_spot_);
      ps.draw_procedural(GPU_PRIM_TRIS, 1, 6);
    }

    screen_pass(copy_ps_, shaders.copy.get());
    copy_ps_.bind_texture("source_tx", &copy_src_);
    copy_ps_.push_constant("uv_transform", &uv_transform_);
    copy_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);

    screen_pass(output_ps_, shaders.output.get());
    output_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_ALWAYS);
    output_ps_.bind_texture("source_tx", &mean_tx_);
    output_ps_.bind_texture("depth_tx", &view_depth_tx_);
    output_ps_.push_constant("uv_transform", &uv_transform_);
    output_ps_.push_constant("depth_proj", &depth_proj_);
    output_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 6);
  }

  /** Index of the material in the table, its data is read on first use. */
  int material_index_get(const Material *material)
  {
    return material_indices_.lookup_or_add_cb(material, [&]() {
      MaterialData data = material_data_get(material);
      if (data.texture && *data.texture && textures_.add(*data.texture)) {
        texture_bytes_ += int64_t(GPU_texture_width(*data.texture)) *
                          GPU_texture_height(*data.texture) * 4 * 4 / 3;
      }
      materials_.append(data);
      return int(materials_.size() - 1);
    });
  }

  PassMain::Sub &material_pass_get(int index, bool textured, bool back)
  {
    return *material_passes_.lookup_or_add_cb(int3(index, textured, back), [&]() {
      const MaterialData &data = materials_[index];
      PassMain::Sub &sub = gbuffer_ps_.sub("ember.material");
      sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                    (back ? DRW_STATE_CULL_FRONT : DRW_STATE_CULL_BACK));
      sub.shader_set(ShaderCache::get().gbuffer[textured][back].get());
      sub.push_constant("mat_base", float4(data.base_color, data.alpha));
      const float table_index = float(math::min(index, material_table_len - 1)) / 255.0f;
      sub.push_constant(
          "mat_params",
          float4(data.metallic, math::clamp(data.roughness, 0.045f, 1.0f), data.f0, table_index));
      if (textured) {
        sub.bind_texture("base_tx", data.texture, data.sampler);
      }
      return &sub;
    });
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
    Span<gpu::Batch *> batches;
    if (has_uv) {
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
    for (const int i : IndexRange(math::min(material_len, int(batches.size())))) {
      if (batches[i] == nullptr) {
        continue;
      }
      const Material *material = BKE_object_material_get_eval(ob, i + 1);
      const int index = material_index_get(material);
      const MaterialData &data = materials_[index];
      const bool textured = has_uv && data.texture && *data.texture;
      material_pass_get(index, textured, false).draw(batches[i], handle);
      if (!data.backface_culling) {
        material_pass_get(index, textured, true).draw(batches[i], handle);
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
  }

  /* ------------------------------------------------------------------ */
  /* Memory. */

  /**
   * Choose the tiles and (re)allocate their buffers so they fit the free video memory. Render
   * targets beyond it would be placed in AGP memory by the driver (5 times slower on the Radeon
   * 9600): their write rate is measured, and the tiles shrink when some were.
   */
  void tiles_ensure(int2 size, bool viewport)
  {
    int64_t available = GPU_render_target_budget();
    const bool budget_known = available >= 0;
    if (!budget_known) {
      available = int64_t(96) << 20;
    }
    /* What Ember holds already is available to it. */
    available += int64_t(tile_allocated_.x) * tile_allocated_.y * tile_bytes_per_pixel;
    if (viewport) {
      if (mean_tx_.gpu_texture()) {
        available += int64_t(GPU_texture_width(mean_tx_)) * GPU_texture_height(mean_tx_) *
                     view_bytes_per_pixel;
      }
      available -= int64_t(size.x) * size.y * view_bytes_per_pixel;
    }
    /* Keep video memory for the textures of the scene too. */
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
    TilePlan plan = tile_plan_get(size, available / tile_bytes_per_pixel);
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
      plan = tile_plan_get(size, available / tile_bytes_per_pixel);
    }
    plan_ = plan;
    budget_ = available;
    printf("Ember: %dx%d in %d tile%s of %dx%d (%d MB of video memory for them)\n",
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
    gb_normal_tx_.ensure_2d(gpu::TextureFormat::UNORM_10_10_10_2, size, usage);
    gb_depth_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32, size, usage);
    gb_specular_tx_.ensure_2d(gpu::TextureFormat::UNORM_8_8_8_8, size, usage);
    depth_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8, size, usage);
    for (int i = 0; i < 2; i++) {
      accum_tx_[i].ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, size, usage);
      accum_fb_[i].ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(accum_tx_[i]));
    }
    gbuffer_fb_.ensure(GPU_ATTACHMENT_TEXTURE(depth_tx_),
                       GPU_ATTACHMENT_TEXTURE(gb_diffuse_tx_),
                       GPU_ATTACHMENT_TEXTURE(gb_normal_tx_),
                       GPU_ATTACHMENT_TEXTURE(gb_depth_tx_),
                       GPU_ATTACHMENT_TEXTURE(gb_specular_tx_));
    tile_allocated_ = size;
  }

  void tile_buffers_free()
  {
    gb_diffuse_tx_.free();
    gb_normal_tx_.free();
    gb_depth_tx_.free();
    gb_specular_tx_.free();
    depth_tx_.free();
    accum_tx_[0].free();
    accum_tx_[1].free();
    tile_allocated_ = int2(0);
  }

  /** False when some tile buffer writes at AGP memory speed. */
  bool tile_buffers_in_video_memory()
  {
    gpu::Texture *textures[] = {
        gb_diffuse_tx_, gb_normal_tx_, gb_depth_tx_, gb_specular_tx_, accum_tx_[0], accum_tx_[1]};
    double rates[6];
    double best = 0.0;
    for (int i = 0; i < 6; i++) {
      rates[i] = GPU_texture_write_rate(textures[i]);
      best = math::max(best, rates[i]);
    }
    if (best <= 0.0) {
      return true;
    }
    /* Video memory writes at 4 GB/s and more on the Radeon 9600, AGP memory at about 1. */
    const double reference = math::max(best, 3.0e9);
    for (int i = 0; i < 6; i++) {
      if (rates[i] < reference * 0.5) {
        printf("Ember: tile buffer %d writes at %.1f GB/s (system memory), shrinking the tiles\n",
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
   * Render one sample of a tile into the accumulation buffers. `mix` is the weight of this
   * sample in the mean. The viewport mean is read from `mean_tx_`, the final render's from the
   * accumulation buffer.
   */
  void render_sample(Manager &manager,
                     const float4x4 &viewmat,
                     const float4x4 &winmat,
                     int2 size,
                     int4 tile,
                     int sample,
                     float2 offset,
                     float mix,
                     bool viewport)
  {
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
    GPU_framebuffer_bind(gbuffer_fb_);
    GPU_framebuffer_viewport_set(gbuffer_fb_, 0, 0, tile.z, tile.w);
    const std::array<double4, 4> clear = {
        double4(0.0), double4(0.0), double4(0.0), double4(0.0)};
    GPU_framebuffer_multi_clear(gbuffer_fb_, clear);
    GPU_framebuffer_clear_depth_stencil(gbuffer_fb_, 1.0f, 0x00);
    manager.submit(gbuffer_ps_, view_);

    /* Screen passes cover the used part of the buffers: their rectangles are in the tile's
     * normalized coordinates, mapped by the viewport. */
    auto bind = [&](Framebuffer &fb) {
      GPU_framebuffer_bind(fb);
      GPU_framebuffer_viewport_set(fb, 0, 0, tile.z, tile.w);
    };

    /* World, emission, background. */
    rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    const float3x3 rotation = float3x3(viewmat);
    world_sh_[0] = float4(world_.sh0, transparent_ ? 1.0f : 0.0f);
    for (int axis = 0; axis < 3; axis++) {
      float3 sh(0.0f);
      for (int j = 0; j < 3; j++) {
        sh += world_.sh1[j] * rotation[j][axis];
      }
      world_sh_[axis + 1] = float4(sh, 0.0f);
    }
    background_ = float4(world_.background, mix);
    if (viewport) {
      /* From the texture coordinates of the tile buffers to the view's. */
      accum_src_ = mean_tx_;
      const float2 mean_size(GPU_texture_width(mean_tx_), GPU_texture_height(mean_tx_));
      accum_uv_ = float4(float2(tile_allocated_) / mean_size, float2(tile.x, tile.y) / mean_size);
    }
    else {
      accum_src_ = accum_tx_[cur_];
      accum_uv_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
    }
    bind(accum_fb_[1 - cur_]);
    manager.submit(base_ps_);
    cur_ = 1 - cur_;

    /* Lights. */
    const float3x3 view_rotation = float3x3(viewmat);
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
      light_color_ = float4(light.intensity * (mix * float(M_1_PI)),
                            light.specular_factor * 0.25f);
      const float smooth = math::max(light.spot_smooth, 1e-4f);
      light_spot_ = float4(
          1.0f / smooth, -light.spot_cos_half / smooth, light.diffuse_factor, 0.0f);
      rect_ = rect;

      accum_src_ = accum_tx_[cur_];
      bind(accum_fb_[1 - cur_]);
      manager.submit(light_ps_[int(light.type)]);
      copy_src_ = accum_tx_[1 - cur_];
      uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
      bind(accum_fb_[cur_]);
      manager.submit(copy_ps_);
    }
  }

  /** Weight of a sample's pixel offset, and the offset. */
  float2 sample_offset(int sample, float &r_weight) const
  {
    if (sample == 0) {
      r_weight = 1.0f;
      return float2(0.0f);
    }
    const float2 u(halton(sample, 2), halton(sample, 3));
    r_weight = blackman_harris(u.x - 0.5f) * blackman_harris(u.y - 0.5f);
    return (u - 0.5f) * math::max(filter_size_, 0.01f);
  }

  /* ------------------------------------------------------------------ */
  /* Viewport. */

  void draw(Manager &manager) final
  {
    DefaultTextureList *dtxl = draw_ctx->viewport_texture_list_get();
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
      uint32_t rotation, intensity;
      memcpy(&rotation, &shading.studiolight_rot_z, sizeof(rotation));
      memcpy(&intensity, &shading.studiolight_intensity, sizeof(intensity));
      settings ^= std::hash<std::string>{}(shading.lookdev_light) ^ uint64_t(shading.type) << 2 ^
                  uint64_t(rotation) << 8 ^ uint64_t(intensity) << 24;
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
      if (sample_ == 0) {
        weight_sum_ = 0.0f;
        mean_tx_.clear(float4(0.0f));
        view_depth_tx_.clear(float4(0.0f));
      }
      float weight;
      const float2 offset = sample_offset(sample_, weight);
      weight_sum_ += weight;
      for (const int t : IndexRange(plan_.len())) {
        const int4 tile = plan_.rect(t);
        render_sample(
            manager, viewmat, winmat, size, tile, sample_, offset, weight / weight_sum_, true);
        /* Keep the tile's mean and depth. */
        proj_type_ = float4(proj_type_.x, 0.0f, 1.0f, 1.0f);
        const float2 view_size(size);
        rect_ = float4(float2(tile.x, tile.y) / view_size * 2.0f - 1.0f,
                       float2(tile.x + tile.z, tile.y + tile.w) / view_size * 2.0f - 1.0f);
        const float2 tile_size(tile_allocated_);
        uv_transform_ = float4(view_size / tile_size, -float2(tile.x, tile.y) / tile_size);
        copy_src_ = accum_tx_[cur_];
        GPU_framebuffer_bind(mean_fb_);
        manager.submit(copy_ps_);
        copy_src_ = gb_depth_tx_;
        GPU_framebuffer_bind(view_depth_fb_);
        manager.submit(copy_ps_);
      }
      sample_++;
    }

    /* Output: the mean, and the depth of the (not jittered) view for the overlays. */
    output_fb_.ensure(GPU_ATTACHMENT_TEXTURE(dtxl->depth), GPU_ATTACHMENT_TEXTURE(dtxl->color));
    GPU_framebuffer_bind(output_fb_);
    rect_ = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    proj_type_ = float4(0.0f, 0.0f, 1.0f, 1.0f);
    uv_transform_ = float4(1.0f, 1.0f, 0.0f, 0.0f);
    const bool is_persp = winmat[3][3] == 0.0f;
    depth_proj_ = float4(winmat[2][2], winmat[3][2], is_persp ? 1.0f : 0.0f, 0.0f);
    manager.submit(output_ps_);
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
    tiles_ensure(size, false);

    RenderPass *combined = RE_pass_find_by_name(layer, RE_PASSNAME_COMBINED, viewname);
    RenderPass *depth = RE_pass_find_by_name(layer, RE_PASSNAME_DEPTH, viewname);
    const int tiles_len = plan_.len();
    for (const int t : IndexRange(tiles_len)) {
      const int4 tile = plan_.rect(t);
      accum_tx_[0].clear(float4(0.0f));
      accum_tx_[1].clear(float4(0.0f));
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
      if (getenv("EMBER_DEBUG")) {
        debug_print(tile);
      }
      if (combined) {
        tile_read(accum_tx_[cur_], tile, 4, combined->ibuf->float_data_for_write(), size, false);
      }
      if (depth) {
        tile_read(gb_depth_tx_, tile, 1, depth->ibuf->float_data_for_write(), size, true);
      }
    }
  }

  /** `EMBER_DEBUG`: what the passes got, for a final render tile. */
  void debug_print(int4 tile)
  {
    printf("Ember debug: world %f %f %f, background %f %f %f, %d lights, %d materials\n",
           world_.sh0.x,
           world_.sh0.y,
           world_.sh0.z,
           world_.background.x,
           world_.background.y,
           world_.background.z,
           int(lights_.size()),
           int(materials_.size()));
    gpu::Texture *textures[] = {gb_diffuse_tx_, gb_normal_tx_, gb_depth_tx_, gb_specular_tx_, accum_tx_[cur_]};
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

}  // namespace ember

/* -------------------------------------------------------------------- */
/** \name Render Engine
 * \{ */

static void ember_render_to_image(RenderEngine *engine, RenderLayer *layer, const rcti /*rect*/)
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

  ember::Instance instance;
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

static void ember_render_update_passes(RenderEngine *engine, Scene *scene, ViewLayer *view_layer)
{
  if (view_layer->passflag & SCE_PASS_COMBINED) {
    RE_engine_register_pass(engine, scene, view_layer, RE_PASSNAME_COMBINED, 4, "RGBA", SOCK_RGBA);
  }
  if (view_layer->passflag & SCE_PASS_DEPTH) {
    RE_engine_register_pass(engine, scene, view_layer, RE_PASSNAME_DEPTH, 1, "Z", SOCK_FLOAT);
  }
}

static void ember_render(RenderEngine *engine, Depsgraph *depsgraph)
{
  DRW_render_to_image(engine, depsgraph, ember_render_to_image, [](RenderResult *) {});
}

RenderEngineType DRW_engine_viewport_ember_type = {
    /*next*/ nullptr,
    /*prev*/ nullptr,
    /*idname*/ "BLENDER_EMBER",
    /*name*/ N_("Ember"),
    /*flag*/ RE_INTERNAL | RE_USE_PREVIEW | RE_USE_GPU_CONTEXT,
    /*update*/ nullptr,
    /*render*/ &ember_render,
    /*render_frame_finish*/ nullptr,
    /*draw*/ nullptr,
    /*bake*/ nullptr,
    /*view_update*/ nullptr,
    /*view_draw*/ nullptr,
    /*update_script_node*/ nullptr,
    /*update_render_passes*/ &ember_render_update_passes,
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
