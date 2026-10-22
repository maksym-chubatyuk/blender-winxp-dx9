/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstdio>
#include <cstring>
#include <mutex>

#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "MEM_guardedalloc.h"

#include "d3d9_backend.hh"
#include "d3d9_buffers.hh"
#include "d3d9_context.hh"
#include "d3d9_shader.hh"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.d3d9.shader"};

using namespace blender::gpu::shader;

/* -------------------------------------------------------------------- */
/** \name Binaries
 * \{ */

const D3D9ShaderBinary *d3d9_shader_binary_find(StringRefNull name)
{
  for (int i = 0; i < d3d9_shader_binaries_len; i++) {
    if (name == d3d9_shader_binaries[i].name) {
      return &d3d9_shader_binaries[i];
    }
  }
  return nullptr;
}

/** Attributes of the HLSL vertex shader, "name:location" separated by spaces. */
static Vector<std::pair<std::string, int>> binary_attributes(const D3D9ShaderBinary *binary)
{
  Vector<std::pair<std::string, int>> attrs;
  if (binary == nullptr || binary->attrs == nullptr) {
    return attrs;
  }
  StringRef str = binary->attrs;
  while (!str.is_empty()) {
    const int64_t end = str.find(' ');
    const StringRef item = end == StringRef::not_found ? str : str.substr(0, end);
    const int64_t colon = item.find(':');
    if (colon != StringRef::not_found) {
      attrs.append({std::string(item.substr(0, colon)), atoi(std::string(item.substr(colon + 1)).c_str())});
    }
    if (end == StringRef::not_found) {
      break;
    }
    str = str.substr(end + 1);
  }
  return attrs;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bytecode parsing
 * \{ */

constexpr uint32_t D3D9_FOURCC_CTAB = 0x42415443; /* "CTAB" */
constexpr uint32_t D3D9_REGSET_FLOAT4 = 2;
constexpr uint32_t D3D9_REGSET_SAMPLER = 3;

/** Call `fn` for every constant of the constant table of the shader. */
static void constant_table_foreach(
    const uint32_t *code,
    uint32_t size,
    FunctionRef<void(StringRefNull name, uint32_t register_set, int index, int count)> fn)
{
  const uint32_t len = size / 4;
  for (uint32_t i = 1; i < len;) {
    const uint32_t token = code[i];
    if (token == 0x0000FFFF) {
      break;
    }
    if ((token & 0xFFFF) == 0xFFFE) {
      const uint32_t comment_len = (token >> 16) & 0x7FFF;
      if (comment_len >= 8 && code[i + 1] == D3D9_FOURCC_CTAB) {
        const uint8_t *table = reinterpret_cast<const uint8_t *>(&code[i + 2]);
        const uint32_t table_size = (comment_len - 1) * 4;
        uint32_t constants, constant_info;
        memcpy(&constants, table + 12, 4);
        memcpy(&constant_info, table + 16, 4);
        for (uint32_t c = 0; c < constants; c++) {
          const uint8_t *info = table + constant_info + c * 20;
          if (info + 20 > table + table_size) {
            break;
          }
          uint32_t name_offset;
          uint16_t register_set, register_index, register_count;
          memcpy(&name_offset, info, 4);
          memcpy(&register_set, info + 4, 2);
          memcpy(&register_index, info + 6, 2);
          memcpy(&register_count, info + 8, 2);
          if (name_offset >= table_size) {
            continue;
          }
          fn(reinterpret_cast<const char *>(table + name_offset),
             register_set,
             register_index,
             register_count);
        }
      }
      i += 1 + comment_len;
      continue;
    }
    i += 1 + ((token >> 24) & 0xF);
  }
}

void D3D9Shader::parse_vertex_inputs(const uint32_t *code, uint32_t size)
{
  constexpr uint32_t D3DSIO_DCL_ = 31;
  constexpr uint32_t D3DSPR_INPUT_ = 1;
  const uint32_t len = size / 4;
  for (uint32_t i = 1; i < len;) {
    const uint32_t token = code[i];
    if (token == 0x0000FFFF) {
      break;
    }
    if ((token & 0xFFFF) == 0xFFFE) {
      i += 1 + ((token >> 16) & 0x7FFF);
      continue;
    }
    if ((token & 0xFFFF) == D3DSIO_DCL_ && i + 2 < len) {
      const uint32_t usage_token = code[i + 1];
      const uint32_t dst = code[i + 2];
      const uint32_t register_type = ((dst >> 28) & 0x7) | ((dst >> 8) & 0x18);
      const uint32_t usage = usage_token & 0x1F;
      const uint32_t usage_index = (usage_token >> 16) & 0xF;
      if (register_type == D3DSPR_INPUT_ && usage == D3DDECLUSAGE_TEXCOORD) {
        vs_inputs_ |= 1 << usage_index;
      }
    }
    i += 1 + ((token >> 24) & 0xF);
  }
  uses_vertex_id_ = (vs_inputs_ & (1 << D3D9_VERTEX_ID_ATTR)) != 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Interface
 * \{ */

D3D9ShaderInterface::D3D9ShaderInterface(const ShaderCreateInfo &info,
                                         const D3D9ShaderBinary *binary)
{
  const Vector<std::pair<std::string, int>> hlsl_attrs = binary_attributes(binary);
  Vector<ShaderCreateInfo::Resource> all_resources = info.resources_get_all_();

  attr_len_ = hlsl_attrs.size();
  uniform_len_ = info.push_constants_.size();
  constant_len_ = info.specialization_constants_.size();
  ubo_len_ = 0;
  ssbo_len_ = 0;

  size_t names_size = 0;
  for (const auto &attr : hlsl_attrs) {
    names_size += attr.first.size() + 1;
  }
  for (const ShaderCreateInfo::PushConst &uni : info.push_constants_) {
    names_size += uni.name.size() + 1;
  }
  for (const SpecializationConstant &constant : info.specialization_constants_) {
    names_size += constant.name.size() + 1;
  }
  for (const ShaderCreateInfo::Resource &res : all_resources) {
    switch (res.bind_type) {
      case ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER:
        ubo_len_++;
        names_size += res.uniformbuf.name.size() + 1;
        break;
      case ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER:
        ssbo_len_++;
        names_size += res.storagebuf.name.size() + 1;
        break;
      case ShaderCreateInfo::Resource::BindType::SAMPLER:
        uniform_len_++;
        names_size += res.sampler.name.size() + 1;
        break;
      case ShaderCreateInfo::Resource::BindType::IMAGE:
        uniform_len_++;
        names_size += res.image.name.size() + 1;
        break;
    }
  }

  const int input_tot_len = attr_len_ + ubo_len_ + uniform_len_ + ssbo_len_ + constant_len_;
  inputs_ = MEM_new_array_zeroed<ShaderInput>(max_ii(input_tot_len, 1), __func__);
  ShaderInput *input = inputs_;
  name_buffer_ = MEM_new_array_uninitialized<char>(max_zz(names_size, 1), "name_buffer");
  uint32_t name_buffer_offset = 0;

  /* Attributes, as declared by the HLSL vertex shader. */
  for (const auto &attr : hlsl_attrs) {
    copy_input_name(input, attr.first, name_buffer_, name_buffer_offset);
    input->location = input->binding = attr.second;
    enabled_attr_mask_ |= (1 << attr.second);
    attr_types_[attr.second] = uint8_t(Type::float4_t);
    for (const ShaderCreateInfo::VertIn &vert_in : info.vertex_inputs_) {
      if (vert_in.name == attr.first) {
        attr_types_[attr.second] = uint8_t(vert_in.type);
      }
    }
    input++;
  }

  /* Uniform blocks. */
  for (const ShaderCreateInfo::Resource &res : all_resources) {
    if (res.bind_type == ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER) {
      copy_input_name(input, res.uniformbuf.name, name_buffer_, name_buffer_offset);
      input->location = input->binding = res.slot;
      enabled_ubo_mask_ |= (1 << res.slot);
      input++;
    }
  }

  /* Samplers & images: locations outside of the push constant range. */
  for (const ShaderCreateInfo::Resource &res : all_resources) {
    if (res.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER) {
      copy_input_name(input, res.sampler.name, name_buffer_, name_buffer_offset);
      input->location = -1;
      input->binding = res.slot;
      enabled_tex_mask_ |= (1ull << res.slot);
      input++;
    }
    else if (res.bind_type == ShaderCreateInfo::Resource::BindType::IMAGE) {
      copy_input_name(input, res.image.name, name_buffer_, name_buffer_offset);
      input->location = -1;
      input->binding = res.slot;
      input++;
    }
  }
  /* Push constants, the location is the index in the create info. */
  int location = 0;
  for (const ShaderCreateInfo::PushConst &uni : info.push_constants_) {
    copy_input_name(input, uni.name, name_buffer_, name_buffer_offset);
    input->location = location++;
    input->binding = -1;
    input++;
  }
  set_image_formats_from_info(info);

  /* Storage buffers. */
  for (const ShaderCreateInfo::Resource &res : all_resources) {
    if (res.bind_type == ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER) {
      copy_input_name(input, res.storagebuf.name, name_buffer_, name_buffer_offset);
      input->location = input->binding = res.slot;
      enabled_ssbo_mask_ |= (1 << res.slot);
      input++;
    }
  }

  /* Constants. */
  int constant_id = 0;
  for (const SpecializationConstant &constant : info.specialization_constants_) {
    copy_input_name(input, constant.name, name_buffer_, name_buffer_offset);
    input->location = constant_id++;
    input++;
  }

  this->sort_inputs();

  for (int32_t u_int = 0; u_int < GPU_NUM_UNIFORMS; u_int++) {
    GPUUniformBuiltin u = static_cast<GPUUniformBuiltin>(u_int);
    const ShaderInput *uni = this->uniform_get(builtin_uniform_name(u));
    builtins_[u] = (uni != nullptr) ? uni->location : -1;
  }
  for (int32_t u_int = 0; u_int < GPU_NUM_UNIFORM_BLOCKS; u_int++) {
    GPUUniformBlockBuiltin u = static_cast<GPUUniformBlockBuiltin>(u_int);
    const ShaderInput *block = this->ubo_get(builtin_uniform_block_name(u));
    builtin_blocks_[u] = (block != nullptr) ? block->binding : -1;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader
 * \{ */

D3D9Shader::D3D9Shader(const char *name) : Shader(name) {}

D3D9Shader::~D3D9Shader()
{
  D3D9Context *ctx = D3D9Context::get();
  if (ctx && ctx->shader == this) {
    this->unbind();
    ctx->shader = nullptr;
  }
  d3d9_release(vs_);
  d3d9_release(ps_);
}

/** Debugging aid: `BLENDER_D3D9_SKIP=name1,name2` disables these shaders (their draws). */
static bool shader_skipped(StringRefNull name)
{
  const char *skip = getenv("BLENDER_D3D9_SKIP");
  if (skip == nullptr) {
    return false;
  }
  StringRef list = skip;
  while (!list.is_empty()) {
    const int64_t comma = list.find(',');
    const StringRef item = comma == StringRef::not_found ? list : list.substr(0, comma);
    if (item == name) {
      return true;
    }
    if (comma == StringRef::not_found) {
      break;
    }
    list = list.substr(comma + 1);
  }
  return false;
}

void D3D9Shader::init(const ShaderCreateInfo &info, bool /*is_codegen_only*/)
{
  const D3D9ShaderBinary *binary = shader_skipped(info.name_) ? nullptr :
                                                                d3d9_shader_binary_find(info.name_);
  interface = new D3D9ShaderInterface(info, binary);

  for (const ShaderCreateInfo::PushConst &uni : info.push_constants_) {
    uniform_names_.append(uni.name);
  }
  uniforms_.resize(uniform_names_.size());

  if (binary == nullptr) {
    /* Report each missing shader once, they are the next ones to write. */
    static Set<std::string> reported;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (reported.add(info.name_)) {
      CLOG_WARN(&LOG, "No Direct3D 9 version of shader \"%s\", skipping its draws", name);
    }
    return;
  }

  IDirect3DDevice9 *device = D3D9Backend::get()->device;
  if (!D3D9_CHECK(device->CreateVertexShader(reinterpret_cast<const DWORD *>(binary->vs), &vs_)) ||
      !D3D9_CHECK(device->CreatePixelShader(reinterpret_cast<const DWORD *>(binary->ps), &ps_)))
  {
    CLOG_ERROR(&LOG, "Shader \"%s\" was rejected by the driver", name);
    d3d9_release(vs_);
    d3d9_release(ps_);
    return;
  }

  parse_vertex_inputs(binary->vs, binary->vs_size);
  parse_instance_data(*binary);
  parse_constant_table(binary->vs, binary->vs_size, true);
  parse_constant_table(binary->ps, binary->ps_size, false);

  /* Uniform buffers are written to the constants named after the block, or to constants named
   * `<block>__<register>` receiving the buffer from this register on (pixel shaders only have 32
   * constants). */
  for (const ShaderCreateInfo::Resource &res : info.resources_get_all_()) {
    if (res.bind_type != ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER) {
      continue;
    }
    /* Strip the array size of the block name. */
    StringRef block_name = res.uniformbuf.name;
    const int64_t bracket = block_name.find('[');
    if (bracket != StringRef::not_found) {
      block_name = block_name.substr(0, bracket);
    }
    std::string types;
    if (binary->ubo_types) {
      const std::string prefix = std::string(block_name) + ":";
      StringRef all_types = binary->ubo_types;
      const int64_t pos = all_types.find(prefix);
      if (pos != StringRef::not_found) {
        StringRef rest = all_types.substr(pos + prefix.size());
        const int64_t type_end = rest.find(' ');
        types = type_end == StringRef::not_found ? rest : rest.substr(0, type_end);
      }
    }
    /* Constant name to block register offset, gathered from both stages. */
    Map<std::string, UniformBlock> blocks;
    auto find = [&](const uint32_t *code, uint32_t size, bool is_vertex) {
      constant_table_foreach(
          code, size, [&](StringRefNull name, uint32_t register_set, int index, int count) {
            if (register_set != D3D9_REGSET_FLOAT4 || !name.startswith(block_name)) {
              return;
            }
            const StringRef suffix = name.substr(block_name.size());
            int src_offset = 0;
            if (!suffix.is_empty()) {
              if (!suffix.startswith("__")) {
                return;
              }
              src_offset = atoi(std::string(suffix.substr(2)).c_str());
            }
            UniformBlock &block = blocks.lookup_or_add_default(name);
            block.slot = res.slot;
            block.src_offset = src_offset;
            block.types = types;
            if (is_vertex) {
              block.vs_reg = int16_t(index);
              block.vs_len = int16_t(count);
            }
            else {
              block.ps_reg = int16_t(index);
              block.ps_len = int16_t(count);
            }
          });
    };
    find(binary->vs, binary->vs_size, true);
    find(binary->ps, binary->ps_size, false);
    for (const UniformBlock &block : blocks.values()) {
      ubo_blocks_.append(block);
      if (block.vs_reg >= 0) {
        vs_constants_.resize(max_ii(vs_constants_.size(), block.vs_reg + block.vs_len),
                             float4(0.0f));
      }
      if (block.ps_reg >= 0) {
        ps_constants_.resize(max_ii(ps_constants_.size(), block.ps_reg + block.ps_len),
                             float4(0.0f));
      }
    }
  }
}

void D3D9Shader::parse_instance_data(const D3D9ShaderBinary &binary)
{
  if (binary.instance_data == nullptr || binary.instance_data[0] == '\0') {
    return;
  }
  StringRef list = binary.instance_data;
  while (!list.is_empty()) {
    const int64_t end = list.find(';');
    const std::string annotation = end == StringRef::not_found ? list : list.substr(0, end);
    list = end == StringRef::not_found ? StringRef() : list.substr(end + 1);

    char key[32], name[64], index[16], types[256];
    int slot, stride, offset;
    if (sscanf(annotation.c_str(), "%31s", key) != 1) {
      continue;
    }
    if (STREQ(key, "vertex-pull")) {
      sscanf(annotation.c_str(), "%*s %d %d", &vertex_pull_slot_, &vertex_pull_stride_);
      continue;
    }
    if (STREQ(key, "point-sprite")) {
      point_sprite_ = true;
      continue;
    }
    if (STREQ(key, "resource-id")) {
      sscanf(annotation.c_str(), "%*s %d %d", &resource_id_slot_, &resource_id_stride_);
      continue;
    }
    if (!STREQ(key, "instance") ||
        sscanf(annotation.c_str(),
               "%*s %63s %d %d %d %15s %255s",
               name,
               &slot,
               &stride,
               &offset,
               index,
               types) != 6)
    {
      CLOG_ERROR(&LOG, "Shader \"%s\": invalid annotation \"%s\"", this->name, annotation.c_str());
      continue;
    }
    InstanceData data;
    data.slot = slot;
    data.stride = stride;
    data.offset = offset;
    data.index_source = STREQ(index, "custom") ? 1 : (STREQ(index, "instance") ? 2 : 0);
    data.types = types;
    auto find = [&](const uint32_t *code, uint32_t size, int16_t &r_reg, int16_t &r_len) {
      constant_table_foreach(
          code, size, [&](StringRefNull const_name, uint32_t register_set, int reg, int count) {
            if (register_set == D3D9_REGSET_FLOAT4 && const_name == name) {
              r_reg = int16_t(reg);
              r_len = int16_t(count);
            }
          });
    };
    find(binary.vs, binary.vs_size, data.vs_reg, data.vs_len);
    find(binary.ps, binary.ps_size, data.ps_reg, data.ps_len);
    if (data.vs_reg < 0 && data.ps_reg < 0) {
      /* Optimized out. */
      continue;
    }
    if (data.vs_reg >= 0) {
      vs_constants_.resize(max_ii(vs_constants_.size(), data.vs_reg + data.vs_len), float4(0.0f));
    }
    if (data.ps_reg >= 0) {
      ps_constants_.resize(max_ii(ps_constants_.size(), data.ps_reg + data.ps_len), float4(0.0f));
    }
    instance_data_.append(std::move(data));
  }
}

void D3D9Shader::instance_data_set(int instance)
{
  D3D9Context *ctx = D3D9Context::get();
  /* Resource and custom ids of the instance, from the draw manager resource id buffer. */
  uint32_t resource_id = 0, custom_id = 0;
  if (resource_id_slot_ >= 0) {
    const D3D9StorageBuf *ids = ctx->bound_ssbos[resource_id_slot_];
    if (ids) {
      const size_t index = size_t(instance) * resource_id_stride_;
      const uint32_t *words = reinterpret_cast<const uint32_t *>(ids->data_get());
      const size_t words_len = ids->size_get() / 4;
      if (index < words_len) {
        resource_id = words[index];
      }
      if (resource_id_stride_ > 1 && index + 1 < words_len) {
        custom_id = words[index + 1];
      }
    }
  }

  static const bool trace = getenv("BLENDER_D3D9_TRACE") != nullptr;
  if (trace) {
    printf("  instance %d resource_id=%u custom_id=%u (ids slot %d %s)\n",
           instance,
           resource_id,
           custom_id,
           resource_id_slot_,
           (resource_id_slot_ >= 0 && ctx->bound_ssbos[resource_id_slot_]) ? "bound" : "unbound");
  }
  for (const InstanceData &data : instance_data_) {
    const D3D9StorageBuf *buf = (data.slot >= 0 && data.slot < D3D9_MAX_SSBO_SLOTS) ?
                                    ctx->bound_ssbos[data.slot] :
                                    nullptr;
    if (trace) {
      printf("    data slot %d vs_reg %d ps_reg %d src %d: %s size %d\n",
             data.slot,
             data.vs_reg,
             data.ps_reg,
             data.index_source,
             buf ? "bound" : "unbound",
             buf ? int(buf->size_get()) : 0);
    }
    if (buf == nullptr) {
      continue;
    }
    const size_t element = data.index_source == 1 ? custom_id :
                           data.index_source == 2 ? size_t(instance) :
                                                    resource_id;
    const size_t start = element * data.stride + data.offset;
    const size_t words_len = data.types.size();
    if (start + words_len * 4 > buf->size_get()) {
      continue;
    }
    const uint32_t *src = reinterpret_cast<const uint32_t *>(buf->data_get() + start);
    /* Convert to floats, see `d3d9_common.hlsl` for the types. */
    float values[128];
    int len = 0;
    for (size_t w = 0; w < words_len && len < 124; w++) {
      switch (data.types[w]) {
        case 'i':
          values[len++] = float(int32_t(src[w]));
          break;
        case 'u':
          values[len++] = float(src[w]);
          break;
        case 'b':
          values[len++] = src[w] ? 1.0f : 0.0f;
          break;
        case 'B':
          for (int byte = 0; byte < 4; byte++) {
            values[len++] = float((src[w] >> (byte * 8)) & 0xFFu) / 255.0f;
          }
          break;
        default:
          memcpy(&values[len++], &src[w], sizeof(float));
          break;
      }
    }
    const int regs = (len + 3) / 4;
    while (len < regs * 4) {
      values[len++] = 0.0f;
    }
    if (trace) {
      printf("    slot %d element %u raw %08x values (%g %g %g %g)\n",
             data.slot,
             uint(element),
             src[0],
             values[0],
             values[1],
             values[2],
             values[3]);
    }
    auto write = [&](int reg, int reg_len, Vector<float4> &constants, int &dirty_min, int &dirty_max) {
      if (reg < 0) {
        return;
      }
      const int count = min_ii(reg_len, regs);
      memcpy(&constants[reg], values, count * sizeof(float4));
      dirty_min = min_ii(dirty_min, reg);
      dirty_max = max_ii(dirty_max, reg + count - 1);
    };
    write(data.vs_reg, data.vs_len, vs_constants_, vs_dirty_min_, vs_dirty_max_);
    write(data.ps_reg, data.ps_len, ps_constants_, ps_dirty_min_, ps_dirty_max_);
  }
}

void D3D9Shader::parse_constant_table(const uint32_t *code, uint32_t size, bool is_vertex)
{
  Vector<float4> &constants = is_vertex ? vs_constants_ : ps_constants_;
  constant_table_foreach(
      code, size, [&](StringRefNull name, uint32_t register_set, int index, int count) {
        if (register_set != D3D9_REGSET_FLOAT4) {
          return;
        }
        for (int i = 0; i < uniform_names_.size(); i++) {
          if (uniform_names_[i] == name) {
            Uniform &uniform = uniforms_[i];
            if (is_vertex) {
              uniform.vs_reg = int16_t(index);
              uniform.vs_len = int16_t(count);
            }
            else {
              uniform.ps_reg = int16_t(index);
              uniform.ps_len = int16_t(count);
            }
          }
        }
        if (index + count > constants.size()) {
          constants.resize(index + count, float4(0.0f));
        }
      });
}

void D3D9Shader::bind(const shader::SpecializationConstants * /*constants_state*/)
{
  D3D9Context *ctx = D3D9Context::get();
  ctx->device->SetVertexShader(vs_);
  ctx->device->SetPixelShader(ps_);
  constants_tag_dirty();
}

void D3D9Shader::unbind()
{
  D3D9Context *ctx = D3D9Context::get();
  if (ctx) {
    ctx->device->SetVertexShader(nullptr);
    ctx->device->SetPixelShader(nullptr);
  }
}

void D3D9Shader::constants_tag_dirty()
{
  vs_dirty_min_ = D3D9_RESERVED_VS_CONSTANTS;
  vs_dirty_max_ = vs_constants_.size() - 1;
  ps_dirty_min_ = 0;
  ps_dirty_max_ = ps_constants_.size() - 1;
}

void D3D9Shader::write_registers(const Uniform &uniform,
                                 int comp_len,
                                 int array_size,
                                 const float *data,
                                 bool is_vertex)
{
  const int base = is_vertex ? uniform.vs_reg : uniform.ps_reg;
  const int len = is_vertex ? uniform.vs_len : uniform.ps_len;
  if (base < 0) {
    return;
  }
  Vector<float4> &constants = is_vertex ? vs_constants_ : ps_constants_;
  /* 3x3 matrices take 3 registers of 3 components, everything else is packed by 4. */
  const int stride = (comp_len == 9) ? 3 : 4;
  const int regs_per_element = (comp_len + stride - 1) / stride;
  int reg_min = INT32_MAX, reg_max = -1;
  for (int e = 0; e < array_size; e++) {
    for (int r = 0; r < regs_per_element; r++) {
      const int reg_offset = e * regs_per_element + r;
      if (reg_offset >= len) {
        break;
      }
      const int reg = base + reg_offset;
      const int comps = min_ii(stride, comp_len - r * stride);
      float *dst = &constants[reg].x;
      memcpy(dst, data + e * comp_len + r * stride, sizeof(float) * comps);
      reg_min = min_ii(reg_min, reg);
      reg_max = max_ii(reg_max, reg);
    }
  }
  if (reg_max < 0) {
    return;
  }
  if (is_vertex) {
    vs_dirty_min_ = min_ii(vs_dirty_min_, reg_min);
    vs_dirty_max_ = max_ii(vs_dirty_max_, reg_max);
  }
  else {
    ps_dirty_min_ = min_ii(ps_dirty_min_, reg_min);
    ps_dirty_max_ = max_ii(ps_dirty_max_, reg_max);
  }
}

void D3D9Shader::uniform_float(int location, int comp_len, int array_size, const float *data)
{
  if (location < 0 || location >= uniforms_.size()) {
    return;
  }
  const Uniform &uniform = uniforms_[location];
  write_registers(uniform, comp_len, array_size, data, true);
  write_registers(uniform, comp_len, array_size, data, false);
}

void D3D9Shader::uniform_int(int location, int comp_len, int array_size, const int *data)
{
  if (location < 0 || location >= uniforms_.size()) {
    return;
  }
  /* SM2.0 has no integer constants usable in expressions: HLSL declares them as floats. */
  const int len = comp_len * array_size;
  float stack_data[64];
  Vector<float> heap_data;
  float *float_data = stack_data;
  if (len > 64) {
    heap_data.resize(len);
    float_data = heap_data.data();
  }
  for (int i = 0; i < len; i++) {
    float_data[i] = float(data[i]);
  }
  uniform_float(location, comp_len, array_size, float_data);
}

void D3D9Shader::constants_upload(IDirect3DDevice9 *device)
{
  /* Copy the bound uniform buffers into their constants. */
  if (!ubo_blocks_.is_empty()) {
    D3D9Context *ctx = D3D9Context::get();
    static const bool trace = getenv("BLENDER_D3D9_TRACE") != nullptr;
    for (const UniformBlock &block : ubo_blocks_) {
      const D3D9UniformBuf *ubo = ctx->bound_ubos[block.slot];
      if (trace) {
        const float *f = ubo ? static_cast<const float *>(ubo->data_get()) + block.src_offset * 4 :
                               nullptr;
        printf("  ubo slot %d offset %d %s first=(%.3f %.3f %.3f %.3f)\n",
               block.slot,
               block.src_offset,
               ubo ? "bound" : "UNBOUND",
               f ? f[0] : 0.0f,
               f ? f[1] : 0.0f,
               f ? f[2] : 0.0f,
               f ? f[3] : 0.0f);
      }
      if (ubo == nullptr) {
        continue;
      }
      const int ubo_regs = int(ubo->size_get() / sizeof(float4)) - block.src_offset;
      if (ubo_regs <= 0) {
        continue;
      }
      const float4 *src = static_cast<const float4 *>(ubo->data_get()) + block.src_offset;
      Vector<float4> converted;
      if (!block.types.empty()) {
        /* Integers become floats: SM2.0 constants are floats only. */
        converted.resize(ubo_regs);
        const uint32_t *raw = reinterpret_cast<const uint32_t *>(src);
        float *dst = &converted[0].x;
        for (int c = 0; c < ubo_regs * 4; c++) {
          const int type_index = block.src_offset * 4 + c;
          const char type = type_index < int(block.types.size()) ? block.types[type_index] : 'f';
          switch (type) {
            case 'i':
              dst[c] = float(int32_t(raw[c]));
              break;
            case 'u':
              dst[c] = float(raw[c]);
              break;
            case 'b':
              dst[c] = raw[c] ? 1.0f : 0.0f;
              break;
            default:
              memcpy(&dst[c], &raw[c], sizeof(float));
              break;
          }
        }
        src = converted.data();
      }
      auto copy = [&](int reg, int len, Vector<float4> &constants, int &dirty_min, int &dirty_max) {
        if (reg < 0) {
          return;
        }
        const int count = min_ii(len, ubo_regs);
        if (memcmp(&constants[reg], src, count * sizeof(float4)) != 0) {
          memcpy(&constants[reg], src, count * sizeof(float4));
          dirty_min = min_ii(dirty_min, reg);
          dirty_max = max_ii(dirty_max, reg + count - 1);
        }
      };
      copy(block.vs_reg, block.vs_len, vs_constants_, vs_dirty_min_, vs_dirty_max_);
      copy(block.ps_reg, block.ps_len, ps_constants_, ps_dirty_min_, ps_dirty_max_);
    }
  }

  if (vs_dirty_max_ >= vs_dirty_min_) {
    device->SetVertexShaderConstantF(
        vs_dirty_min_, &vs_constants_[vs_dirty_min_].x, vs_dirty_max_ - vs_dirty_min_ + 1);
  }
  if (ps_dirty_max_ >= ps_dirty_min_) {
    device->SetPixelShaderConstantF(
        ps_dirty_min_, &ps_constants_[ps_dirty_min_].x, ps_dirty_max_ - ps_dirty_min_ + 1);
  }
  vs_dirty_min_ = ps_dirty_min_ = INT32_MAX;
  vs_dirty_max_ = ps_dirty_max_ = -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compiler
 * \{ */

Shader *D3D9ShaderCompiler::compile_shader(const ShaderCreateInfo &orig_info)
{
  const_cast<ShaderCreateInfo &>(orig_info).finalize();

  ShaderCreateInfo info = orig_info;
  if (!info.compilation_constants_.is_empty()) {
    auto predicate = [&](const ShaderCreateInfo::Resource &res) {
      return !res.conditions.evaluate(info.compilation_constants_);
    };
    info.pass_resources_.remove_if(predicate);
    info.batch_resources_.remove_if(predicate);
    info.geometry_resources_.remove_if(predicate);
  }
  auto cleanup_duplicates = [&](Vector<ShaderCreateInfo::Resource, 0> &resources) {
    Vector<ShaderCreateInfo::Resource, 0> tmp = resources;
    resources.clear();
    resources.extend_non_duplicates(tmp);
  };
  cleanup_duplicates(info.pass_resources_);
  cleanup_duplicates(info.batch_resources_);
  cleanup_duplicates(info.geometry_resources_);

  D3D9Shader *shader = new D3D9Shader(info.name_.c_str());
  shader->specialization_constants_init(info);
  shader->init(info, false);
  shader->fragment_output_bits = 0;
  for (const ShaderCreateInfo::FragOut &frag_out : info.fragment_outputs_) {
    shader->fragment_output_bits |= 1u << frag_out.index;
  }
  return shader;
}

/** \} */

}  // namespace blender::gpu
