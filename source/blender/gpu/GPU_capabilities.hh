/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * GPU Capabilities & workarounds
 * This module expose the reported implementation limits & enabled
 * workaround for drivers that needs specific code-paths.
 */

#pragma once

#include "BLI_sys_types.h"

namespace blender {

namespace gpu {
class Texture;
}

int GPU_max_texture_size();
int GPU_max_texture_3d_size();
uint32_t GPU_max_buffer_texture_size();
int GPU_max_texture_layers();
int GPU_max_textures();
int GPU_max_images();
int GPU_max_work_group_count(int index);
int GPU_max_work_group_size(int index);
int GPU_max_uniforms_vert();
int GPU_max_uniforms_frag();
int GPU_max_batch_indices();
int GPU_max_batch_vertices();
int GPU_max_vertex_attribs();
int GPU_max_varying_floats();
int GPU_max_shader_storage_buffer_bindings();
int GPU_max_compute_shader_storage_blocks();
size_t GPU_max_uniform_buffer_size();
size_t GPU_max_storage_buffer_size();
/* Used when binding subrange of SSBOs. In bytes.
 * The start of the range must be aligned with this value. */
size_t GPU_storage_buffer_alignment();

int GPU_extensions_len();
const char *GPU_extension_get(int i);

int GPU_texture_size_with_limit(int res);

/**
 * Returns whether it should be "safe" to use texture of a given size.
 *
 * The heuristic is that maybe allocating texture that is 25% of
 * #GPU_max_texture_size squared is fine. Note that the actual texture creation
 * can still fail even if deemed "safe" by this function, depending on current memory
 * usage, texture format, etc.
 */
bool GPU_is_safe_texture_size(int width, int height);

bool GPU_use_subprocess_compilation();
int GPU_max_parallel_compilations();

bool GPU_stencil_clasify_buffer_workaround();
bool GPU_depth_blitting_workaround();
bool GPU_use_main_context_workaround();
bool GPU_use_hq_normals_workaround();

bool GPU_geometry_shader_support();
/** False for Shader Model 2.0 hardware (Direct3D 9): the draw manager does its GPU work on the CPU. */
bool GPU_compute_shader_support();
/** Blending into 16 bit floating point render targets (not on R300 class hardware). */
bool GPU_float_blending_support();
bool GPU_hdr_support();
bool GPU_stencil_export_support();

bool GPU_mem_stats_supported();
void GPU_mem_stats_get(int *r_totalmem, int *r_freemem);

/**
 * Bytes of local video memory still free for render targets: the memory on the card minus the
 * display surfaces and the render targets already allocated. -1 when the backend can't tell.
 * Render targets beyond it may be placed in slower system (AGP) memory instead of failing.
 */
int64_t GPU_render_target_budget();
/**
 * Measured write rate of a render target texture in bytes per second (overwrites its content),
 * 0 when the backend can't measure it. Tells render targets placed in system memory, several
 * times slower than video memory.
 */
double GPU_texture_write_rate(gpu::Texture *texture);
/** Move textures that can be restored out of video memory, so render targets created next get
 * it first. */
void GPU_memory_evict_managed();

/**
 * Return support for the active context + window.
 */
bool GPU_stereo_quadbuffer_support();

int GPU_minimum_per_vertex_stride();

/** WARNING: Should only be called at startup from creator_args. Never call it at runtime. */
void GPU_compilation_subprocess_override_set(int count);

}  // namespace blender
