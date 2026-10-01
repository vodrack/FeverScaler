#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace feverscaler {
// Minimal SPIR-V reflection: TF3 compiles its GLSL at runtime with glslang and keeps OpName,
// so programs can be identified by their uniform block and input variable names.
struct SpirvInfo {
  bool valid = false;
  std::vector<uint32_t> code;  // kept for fragment shaders reading posAmbient (hashed-alpha patch)
  std::vector<uint32_t> srCode;  // optional SSR variant, used only in SR pipeline copies
  struct Binding {
    uint32_t set = 0, binding = 0;
    std::string name;      // variable name, e.g. "u_view"
    std::string typeName;  // block type name, e.g. "View"
  };
  std::vector<Binding> bindings;
  std::vector<std::pair<uint32_t, std::string>> inputs;  // (location, name) of stage inputs

  bool HasBinding(const char* varName) const;
  int InputLocation(const char* name) const;  // -1 if absent
};
SpirvInfo ReflectSpirv(const uint32_t* code, size_t sizeBytes);
}  // namespace feverscaler
