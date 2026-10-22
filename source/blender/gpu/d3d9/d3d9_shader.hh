/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_vector.hh"

#include "gpu_shader_interface.hh"
#include "gpu_shader_private.hh"

#include "d3d9_common.hh"

namespace blender::gpu {

/** Precompiled shader, generated from `d3d9/shaders/d3d9_shaders.txt` at build time. */
struct D3D9ShaderBinary {
  /** Name of the replaced #ShaderCreateInfo. */
  const char *name;
  const uint32_t *vs;
  uint32_t vs_size;
  const uint32_t *ps;
  uint32_t ps_size;
  /** Vertex shader attributes: `name:location` pairs separated by spaces. */
  const char *attrs;
  /** Component types of uniform buffers: `name:types` pairs (see `d3d9_common.hlsl`). */
  const char *ubo_types;
  /** Per instance data annotations, separated by ';' (see `d3d9_common.hlsl`). */
  const char *instance_data;
};

extern const D3D9ShaderBinary d3d9_shader_binaries[];
extern const int d3d9_shader_binaries_len;

const D3D9ShaderBinary *d3d9_shader_binary_find(StringRefNull name);

class D3D9ShaderInterface : public ShaderInterface {
 public:
  D3D9ShaderInterface(const shader::ShaderCreateInfo &info, const D3D9ShaderBinary *binary);
};

class D3D9Shader : public Shader {
 public:
  explicit D3D9Shader(const char *name);
  ~D3D9Shader() override;

  void init(const shader::ShaderCreateInfo &info, bool is_codegen_only) override;

  const shader::ShaderCreateInfo &patch_create_info(
      const shader::ShaderCreateInfo &original_info) override
  {
    return original_info;
  }

  void vertex_shader_from_glsl(const shader::ShaderCreateInfo & /*info*/,
                               MutableSpan<StringRefNull> /*sources*/) override
  {
  }
  void geometry_shader_from_glsl(const shader::ShaderCreateInfo & /*info*/,
                                 MutableSpan<StringRefNull> /*sources*/) override
  {
  }
  void fragment_shader_from_glsl(const shader::ShaderCreateInfo & /*info*/,
                                 MutableSpan<StringRefNull> /*sources*/) override
  {
  }
  void compute_shader_from_glsl(const shader::ShaderCreateInfo & /*info*/,
                                MutableSpan<StringRefNull> /*sources*/) override
  {
  }
  bool finalize(const shader::ShaderCreateInfo * /*info*/ = nullptr) override
  {
    return true;
  }
  void warm_cache(int /*limit*/) override {}

  void bind(const shader::SpecializationConstants *constants_state) override;
  void unbind() override;

  void uniform_float(int location, int comp_len, int array_size, const float *data) override;
  void uniform_int(int location, int comp_len, int array_size, const int *data) override;

  std::string resources_declare(const shader::ShaderCreateInfo & /*info*/) const override
  {
    return "";
  }
  std::string vertex_interface_declare(const shader::ShaderCreateInfo & /*info*/) const override
  {
    return "";
  }
  std::string fragment_interface_declare(const shader::ShaderCreateInfo & /*info*/) const override
  {
    return "";
  }
  std::string geometry_interface_declare(const shader::ShaderCreateInfo & /*info*/) const override
  {
    return "";
  }
  std::string geometry_layout_declare(const shader::ShaderCreateInfo & /*info*/) const override
  {
    return "";
  }
  std::string compute_layout_declare(const shader::ShaderCreateInfo & /*info*/) const override
  {
    return "";
  }

  /** False when there is no D3D9 version of this shader: draws using it are skipped. */
  bool is_valid() const
  {
    return vs_ != nullptr && ps_ != nullptr;
  }
  /** Bit-mask of the `TEXCOORD<n>` inputs of the vertex shader. */
  uint16_t vertex_inputs_get() const
  {
    return vs_inputs_;
  }
  bool uses_vertex_id() const
  {
    return uses_vertex_id_;
  }

  /** The shader reads per instance data (object matrices...) from storage buffers. */
  bool has_instance_data() const
  {
    return !instance_data_.is_empty();
  }
  /** Copy the per instance data of an instance to the constants. */
  void instance_data_set(int instance);

  /** First constants of each stage, for debug traces. */
  const float4 *ps_constants_get() const
  {
    return ps_constants_.is_empty() ? nullptr : ps_constants_.data();
  }

  /** Send the modified constants to the device, the shader must be bound. */
  void constants_upload(IDirect3DDevice9 *device);
  /** Constants need to be sent again (shader was re-bound). */
  void constants_tag_dirty();

 private:
  /** Push constant mapping to registers, indexed by uniform location. */
  struct Uniform {
    int16_t vs_reg = -1;
    int16_t vs_len = 0;
    int16_t ps_reg = -1;
    int16_t ps_len = 0;
  };

  void parse_constant_table(const uint32_t *code, uint32_t size, bool is_vertex);
  void parse_vertex_inputs(const uint32_t *code, uint32_t size);
  void write_registers(const Uniform &uniform,
                       int comp_len,
                       int array_size,
                       const float *data,
                       bool is_vertex);

  IDirect3DVertexShader9 *vs_ = nullptr;
  IDirect3DPixelShader9 *ps_ = nullptr;

  /** Uniform buffer copied to the constants named after it. */
  struct UniformBlock {
    int slot = 0;
    /** First register of the buffer copied (constants named `<block>__<register>`). */
    int src_offset = 0;
    /** One character per 32-bit component: f(loat), i(nt), u(int) or b(ool), empty for floats. */
    std::string types;
    int16_t vs_reg = -1;
    int16_t vs_len = 0;
    int16_t ps_reg = -1;
    int16_t ps_len = 0;
  };

  /** Element of a storage buffer copied to constants for each instance. */
  struct InstanceData {
    int16_t vs_reg = -1;
    int16_t vs_len = 0;
    int16_t ps_reg = -1;
    int16_t ps_len = 0;
    int slot = 0;
    int stride = 0;
    int offset = 0;
    /** Element index: 0 resource id, 1 custom id, 2 the instance itself. */
    int index_source = 0;
    std::string types;
  };
  void parse_instance_data(const D3D9ShaderBinary &binary);

  /** Name of each uniform location, to find it in the constant tables. */
  Vector<std::string> uniform_names_;
  Vector<Uniform> uniforms_;
  Vector<UniformBlock> ubo_blocks_;
  Vector<InstanceData> instance_data_;
  /** Storage buffer slot of the resource ids, and number of words per instance. */
  int resource_id_slot_ = -1;
  int resource_id_stride_ = 1;

  /** CPU copy of the constant registers and the dirty register ranges. */
  Vector<float4> vs_constants_;
  Vector<float4> ps_constants_;
  int vs_dirty_min_ = INT32_MAX, vs_dirty_max_ = -1;
  int ps_dirty_min_ = INT32_MAX, ps_dirty_max_ = -1;

  uint16_t vs_inputs_ = 0;
  bool uses_vertex_id_ = false;
};

class D3D9ShaderCompiler : public ShaderCompiler {
 public:
  D3D9ShaderCompiler() : ShaderCompiler(1, GPUWorker::ContextType::Main, false) {}

  /** Skip the GLSL source assembly: the shader only needs its create info and binary. */
  Shader *compile_shader(const shader::ShaderCreateInfo &info) override;
};

}  // namespace blender::gpu
