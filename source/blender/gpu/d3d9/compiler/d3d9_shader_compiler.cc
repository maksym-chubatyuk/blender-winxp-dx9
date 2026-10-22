/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Build time HLSL compiler of the Direct3D 9 backend.
 *
 * Compiles every shader listed in the manifest to `vs_2_0`/`ps_2_0` bytecode with Microsoft's
 * `d3dcompiler_43.dll` (Wine's own compiler produces bytecode that real SM2.0 drivers reject, run
 * this under Wine with the native DLL) and writes the table of #D3D9ShaderBinary.
 *
 * Compiling against the 2.0 profiles also enforces the hardware limits (64 arithmetic and 32
 * texture instructions per pixel shader): a shader exceeding them fails the build.
 *
 * Manifest lines: `<create info name> <file.hlsl> [DEFINE[=value]...]`, `#` starts a comment.
 * Each file has a `vert` and a `frag` entry point, the vertex input structure is named `VertIn`.
 * Uniform buffers containing integers declare their layout with a comment line
 * `// d3d9-ubo <name> <types>`, one character per 32-bit component (f, i, u or b), which lets
 * the backend convert them to floats.
 *
 * Usage: d3d9_shader_compiler <manifest> <hlsl directory> <output.cc> [disassembly directory]
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using D3DCompileFn = decltype(&D3DCompile);
using D3DPreprocessFn = decltype(&D3DPreprocess);
using D3DDisassembleFn = decltype(&D3DDisassemble);

static D3DCompileFn d3d_compile = nullptr;
static D3DPreprocessFn d3d_preprocess = nullptr;
static D3DDisassembleFn d3d_disassemble = nullptr;

static bool read_file(const std::string &path, std::string &r_content)
{
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::stringstream ss;
  ss << file.rdbuf();
  r_content = ss.str();
  return true;
}

/**
 * Turn the `// d3d9-...` annotation comments into `__d3d9_annotation "d3d9-..."` lines, which
 * survive the preprocessor: the annotations of disabled `#if` blocks are then left out.
 */
static std::string annotations_as_code(const std::string &source)
{
  std::string result;
  std::istringstream lines(source);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t start = line.find_first_not_of(" \t");
    if (start != std::string::npos && line.compare(start, 8, "// d3d9-") == 0) {
      std::string annotation = line.substr(start + 3);
      annotation.erase(annotation.find_last_not_of(" \t\r") + 1);
      line = "__d3d9_annotation \"" + annotation + "\"";
    }
    result += line + "\n";
  }
  return result;
}

/** Resolve `#include "file"` relative to the HLSL directory. */
class Includer : public ID3DInclude {
 public:
  explicit Includer(std::string dir, bool annotations = false)
      : dir_(std::move(dir)), annotations_(annotations)
  {
  }

  STDMETHOD(Open)
  (D3D_INCLUDE_TYPE /*type*/,
   LPCSTR file_name,
   LPCVOID /*parent_data*/,
   LPCVOID *r_data,
   UINT *r_bytes) override
  {
    std::string *content = new std::string();
    if (!read_file(dir_ + "/" + file_name, *content)) {
      delete content;
      return E_FAIL;
    }
    if (annotations_) {
      *content = annotations_as_code(*content);
    }
    files_[content->data()] = content;
    *r_data = content->data();
    *r_bytes = UINT(content->size());
    return S_OK;
  }

  STDMETHOD(Close)(LPCVOID data) override
  {
    auto it = files_.find(static_cast<const char *>(data));
    if (it != files_.end()) {
      delete it->second;
      files_.erase(it);
    }
    return S_OK;
  }

 private:
  std::string dir_;
  bool annotations_;
  std::map<const char *, std::string *> files_;
};

struct Entry {
  std::string name;
  std::string file;
  std::vector<std::string> defines;
};

static std::vector<Entry> read_manifest(const std::string &path)
{
  std::vector<Entry> entries;
  std::string content;
  if (!read_file(path, content)) {
    fprintf(stderr, "d3d9_shader_compiler: cannot read %s\n", path.c_str());
    exit(1);
  }
  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    std::istringstream words(line);
    Entry entry;
    if (!(words >> entry.name >> entry.file)) {
      continue;
    }
    std::string define;
    while (words >> define) {
      entry.defines.push_back(define);
    }
    entries.push_back(entry);
  }
  return entries;
}

static std::vector<D3D_SHADER_MACRO> make_macros(const Entry &entry,
                                                 std::vector<std::string> &r_storage)
{
  r_storage.clear();
  for (const std::string &define : entry.defines) {
    const size_t eq = define.find('=');
    r_storage.push_back(define.substr(0, eq));
    r_storage.push_back(eq == std::string::npos ? "1" : define.substr(eq + 1));
  }
  std::vector<D3D_SHADER_MACRO> macros;
  for (size_t i = 0; i < r_storage.size(); i += 2) {
    macros.push_back({r_storage[i].c_str(), r_storage[i + 1].c_str()});
  }
  macros.push_back({nullptr, nullptr});
  return macros;
}

static bool compile(const Entry &entry,
                    const std::string &source,
                    const std::string &dir,
                    const char *entry_point,
                    const char *profile,
                    std::vector<uint32_t> &r_code,
                    const std::string &asm_path)
{
  std::vector<std::string> storage;
  std::vector<D3D_SHADER_MACRO> macros = make_macros(entry, storage);
  Includer includer(dir);
  ID3DBlob *code = nullptr, *errors = nullptr;
  const std::string file_path = dir + "/" + entry.file;
  HRESULT hr = d3d_compile(source.data(),
                           source.size(),
                           file_path.c_str(),
                           macros.data(),
                           &includer,
                           entry_point,
                           profile,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3,
                           0,
                           &code,
                           &errors);
  if (errors) {
    fprintf(stderr, "%s", static_cast<const char *>(errors->GetBufferPointer()));
    errors->Release();
  }
  if (FAILED(hr) || code == nullptr) {
    fprintf(stderr,
            "d3d9_shader_compiler: %s (%s %s) failed\n",
            entry.name.c_str(),
            entry_point,
            profile);
    return false;
  }
  r_code.resize(code->GetBufferSize() / 4);
  memcpy(r_code.data(), code->GetBufferPointer(), r_code.size() * 4);

  /* The compiler doesn't enforce the 2.0 instruction limits, the drivers reject such shaders. */
  if (d3d_disassemble) {
    ID3DBlob *text = nullptr;
    if (SUCCEEDED(d3d_disassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &text)))
    {
      const std::string disassembly(static_cast<const char *>(text->GetBufferPointer()),
                                    text->GetBufferSize());
      text->Release();
      int slots = 0, texture = 0, arithmetic = 0;
      const size_t pos = disassembly.find("// approximately ");
      if (pos != std::string::npos) {
        if (sscanf(disassembly.c_str() + pos,
                   "// approximately %d instruction slots used (%d texture, %d arithmetic)",
                   &slots,
                   &texture,
                   &arithmetic) < 3)
        {
          arithmetic = slots;
        }
      }
      const bool is_pixel = profile[0] == 'p';
      const bool too_long = is_pixel ? (arithmetic > 64 || texture > 32) : (slots > 256);
      if (too_long) {
        fprintf(stderr,
                "d3d9_shader_compiler: %s (%s %s) exceeds the instruction limits: %d slots, %d "
                "texture, %d arithmetic\n",
                entry.name.c_str(),
                entry_point,
                profile,
                slots,
                texture,
                arithmetic);
        code->Release();
        return false;
      }
    }
  }

  if (!asm_path.empty() && d3d_disassemble) {
    ID3DBlob *text = nullptr;
    if (SUCCEEDED(d3d_disassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &text)))
    {
      FILE *f = fopen(asm_path.c_str(), "wb");
      if (f) {
        fwrite(text->GetBufferPointer(), 1, text->GetBufferSize(), f);
        fclose(f);
      }
      text->Release();
    }
  }
  code->Release();
  return true;
}

/** Extract `name:location` of the `VertIn` members from the preprocessed source. */
static std::string vertex_attributes(const Entry &entry,
                                     const std::string &source,
                                     const std::string &dir)
{
  std::vector<std::string> storage;
  std::vector<D3D_SHADER_MACRO> macros = make_macros(entry, storage);
  Includer includer(dir);
  ID3DBlob *text = nullptr, *errors = nullptr;
  if (FAILED(d3d_preprocess(source.data(),
                            source.size(),
                            entry.file.c_str(),
                            macros.data(),
                            &includer,
                            &text,
                            &errors)))
  {
    if (errors) {
      errors->Release();
    }
    return "";
  }
  std::string code(static_cast<const char *>(text->GetBufferPointer()), text->GetBufferSize());
  text->Release();
  if (errors) {
    errors->Release();
  }

  const size_t start = code.find("struct VertIn");
  if (start == std::string::npos) {
    return "";
  }
  const size_t open = code.find('{', start);
  const size_t close = code.find('}', open);
  std::string body;
  {
    /* Without the preprocessor lines (`#line`) between the members. */
    std::istringstream lines(code.substr(open + 1, close - open - 1));
    std::string line;
    while (std::getline(lines, line)) {
      const size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line[start] != '#') {
        body += line + "\n";
      }
    }
  }
  std::string result;
  std::istringstream members(body);
  std::string member;
  while (std::getline(members, member, ';')) {
    const size_t colon = member.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::istringstream decl(member.substr(0, colon));
    std::string type, name;
    decl >> type >> name;
    std::string semantic = member.substr(colon + 1);
    semantic.erase(0, semantic.find_first_not_of(" \t\r\n"));
    semantic.erase(semantic.find_last_not_of(" \t\r\n") + 1);
    if (semantic.compare(0, 8, "TEXCOORD") != 0) {
      continue;
    }
    const int location = atoi(semantic.c_str() + 8);
    if (location == 15) {
      /* Vertex index, not an attribute. */
      continue;
    }
    if (!result.empty()) {
      result += " ";
    }
    result += name + ":" + std::to_string(location);
  }
  return result;
}

/**
 * The annotations of the shader variant (its defines applied), as `// d3d9-...` lines.
 */
static std::string variant_annotations(const Entry &entry,
                                       const std::string &source,
                                       const std::string &dir)
{
  std::vector<std::string> storage;
  std::vector<D3D_SHADER_MACRO> macros = make_macros(entry, storage);
  Includer includer(dir, true);
  const std::string annotated = annotations_as_code(source);
  ID3DBlob *text = nullptr, *errors = nullptr;
  if (FAILED(d3d_preprocess(annotated.data(),
                            annotated.size(),
                            entry.file.c_str(),
                            macros.data(),
                            &includer,
                            &text,
                            &errors)))
  {
    if (errors) {
      fprintf(stderr, "%s", static_cast<const char *>(errors->GetBufferPointer()));
      errors->Release();
    }
    return "";
  }
  const std::string code(static_cast<const char *>(text->GetBufferPointer()),
                         text->GetBufferSize());
  text->Release();
  if (errors) {
    errors->Release();
  }
  std::string result;
  const std::string key = "__d3d9_annotation \"";
  for (size_t pos = code.find(key); pos != std::string::npos; pos = code.find(key, pos)) {
    pos += key.size();
    const size_t end = code.find('"', pos);
    if (end == std::string::npos) {
      break;
    }
    result += "// " + code.substr(pos, end - pos) + "\n";
    pos = end;
  }
  return result;
}

/**
 * The `// d3d9-instance` and `// d3d9-resource-id` annotations (per instance data, see
 * `d3d9_common.hlsl`), without the prefix, separated by ';'.
 */
static std::string instance_annotations(const std::string &source)
{
  std::string result;
  std::istringstream lines(source);
  std::string line;
  while (std::getline(lines, line)) {
    for (const char *key : {"// d3d9-instance ", "// d3d9-resource-id ", "// d3d9-point-sprite", "// d3d9-vertex-pull ", "// d3d9-select-output", "// d3d9-flat-color"}) {
      const size_t pos = line.find(key);
      if (pos == std::string::npos) {
        continue;
      }
      std::string annotation = line.substr(pos + 8);
      annotation.erase(annotation.find_last_not_of(" \t\r") + 1);
      if (!result.empty()) {
        result += ";";
      }
      result += annotation;
    }
  }
  return result;
}

/** `name:types` of the `// d3d9-ubo` annotations of the source, separated by spaces. */
static std::string ubo_types(const std::string &source)
{
  std::string result;
  std::istringstream lines(source);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t pos = line.find("// d3d9-ubo ");
    if (pos == std::string::npos) {
      continue;
    }
    std::istringstream words(line.substr(pos + 12));
    std::string name, types, word;
    words >> name;
    while (words >> word) {
      types += word;
    }
    if (!result.empty()) {
      result += " ";
    }
    result += name + ":" + types;
  }
  return result;
}

static void write_array(FILE *out, const char *name, const std::vector<uint32_t> &code)
{
  fprintf(out, "static const uint32_t %s[] = {", name);
  for (size_t i = 0; i < code.size(); i++) {
    fprintf(out, "%s0x%08x,", (i % 8) ? " " : "\n    ", code[i]);
  }
  fprintf(out, "\n};\n");
}

int main(int argc, char **argv)
{
  if (argc < 4) {
    fprintf(stderr, "usage: %s <manifest> <hlsl directory> <output.cc> [asm directory]\n", argv[0]);
    return 1;
  }
  const std::string manifest = argv[1];
  const std::string dir = argv[2];
  const std::string output = argv[3];
  const std::string asm_dir = argc > 4 ? argv[4] : "";

  HMODULE lib = LoadLibraryA("d3dcompiler_43.dll");
  if (lib == nullptr) {
    fprintf(stderr, "d3d9_shader_compiler: d3dcompiler_43.dll not found\n");
    return 1;
  }
  d3d_compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(lib, "D3DCompile"));
  d3d_preprocess = reinterpret_cast<D3DPreprocessFn>(GetProcAddress(lib, "D3DPreprocess"));
  d3d_disassemble = reinterpret_cast<D3DDisassembleFn>(GetProcAddress(lib, "D3DDisassemble"));
  if (d3d_compile == nullptr || d3d_preprocess == nullptr) {
    fprintf(stderr, "d3d9_shader_compiler: d3dcompiler_43.dll is incomplete\n");
    return 1;
  }

  const std::vector<Entry> entries = read_manifest(manifest);
  std::string table;
  std::string arrays_file = output + ".tmp";
  FILE *out = fopen(arrays_file.c_str(), "wb");
  if (out == nullptr) {
    fprintf(stderr, "d3d9_shader_compiler: cannot write %s\n", output.c_str());
    return 1;
  }
  fprintf(out,
          "/* Generated by d3d9_shader_compiler from d3d9/shaders, do not edit. */\n\n"
          "#include \"d3d9_shader.hh\"\n\n"
          "namespace blender::gpu {\n\n");

  bool ok = true;
  std::map<std::string, std::string> source_cache;
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry &entry = entries[i];
    std::string &source = source_cache[entry.file];
    if (source.empty() && !read_file(dir + "/" + entry.file, source)) {
      fprintf(stderr, "d3d9_shader_compiler: cannot read %s\n", entry.file.c_str());
      ok = false;
      continue;
    }
    std::vector<uint32_t> vs, ps;
    const std::string asm_base = asm_dir.empty() ? "" : asm_dir + "/" + entry.name;
    if (!compile(entry, source, dir, "vert", "vs_2_0", vs, asm_base.empty() ? "" : asm_base + ".vs.asm") ||
        !compile(entry, source, dir, "frag", "ps_2_0", ps, asm_base.empty() ? "" : asm_base + ".ps.asm"))
    {
      ok = false;
      continue;
    }
    const std::string attrs = vertex_attributes(entry, source, dir);
    const std::string full_source = variant_annotations(entry, source, dir);
    char name[64];
    snprintf(name, sizeof(name), "sh%zu_vs", i);
    write_array(out, name, vs);
    snprintf(name, sizeof(name), "sh%zu_ps", i);
    write_array(out, name, ps);
    fprintf(out, "\n");

    char row[4096];
    snprintf(row,
             sizeof(row),
             "    {\"%s\", sh%zu_vs, %zu, sh%zu_ps, %zu, \"%s\", \"%s\", \"%s\"},\n",
             entry.name.c_str(),
             i,
             vs.size() * 4,
             i,
             ps.size() * 4,
             attrs.c_str(),
             ubo_types(full_source).c_str(),
             instance_annotations(full_source).c_str());
    table += row;
  }

  fprintf(out,
          "const D3D9ShaderBinary d3d9_shader_binaries[] = {\n%s    {nullptr, nullptr, 0, "
          "nullptr, 0, nullptr, nullptr, nullptr},\n};\n\n",
          table.c_str());
  fprintf(out,
          "const int d3d9_shader_binaries_len = %zu;\n\n}  // namespace blender::gpu\n",
          entries.size());
  fclose(out);

  if (!ok) {
    DeleteFileA(arrays_file.c_str());
    return 1;
  }
  /* Replace the output only when complete. */
  DeleteFileA(output.c_str());
  if (!MoveFileA(arrays_file.c_str(), output.c_str())) {
    fprintf(stderr, "d3d9_shader_compiler: cannot write %s\n", output.c_str());
    return 1;
  }
  return 0;
}
