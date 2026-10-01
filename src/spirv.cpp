#include "spirv.h"

#include <cstring>
#include <unordered_map>

namespace feverscaler {
bool SpirvInfo::HasBinding(const char* varName) const {
  for (auto& b : bindings)
    if (b.name == varName) return true;
  return false;
}
int SpirvInfo::InputLocation(const char* name) const {
  for (auto& i : inputs)
    if (i.second == name) return (int)i.first;
  return -1;
}

SpirvInfo ReflectSpirv(const uint32_t* code, size_t sizeBytes) {
  SpirvInfo out;
  size_t n = sizeBytes / 4;
  if (!code || n < 5 || code[0] != 0x07230203) return out;
  std::unordered_map<uint32_t, std::string> names;
  std::unordered_map<uint32_t, uint32_t> descSet, binding, location;
  std::unordered_map<uint32_t, uint32_t> ptrPointee;               // OpTypePointer id -> pointee type
  std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> vars;  // var id -> (ptr type, storage class)
  for (size_t i = 5; i < n;) {
    uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
    if (wc == 0 || i + wc > n) break;
    const uint32_t* w = code + i + 1;
    switch (op) {
      case 5: {  // OpName
        const char* s = (const char*)(w + 1);
        size_t maxLen = (wc - 2) * 4;
        names[w[0]] = std::string(s, strnlen(s, maxLen));
        break;
      }
      case 71:  // OpDecorate
        if (wc >= 4) {
          if (w[1] == 34) descSet[w[0]] = w[2];
          else if (w[1] == 33) binding[w[0]] = w[2];
          else if (w[1] == 30) location[w[0]] = w[2];
        }
        break;
      case 32:  // OpTypePointer result, storage, type
        if (wc >= 4) ptrPointee[w[0]] = w[2];
        break;
      case 59:  // OpVariable resultType, result, storage
        if (wc >= 4) vars.push_back({w[1], {w[0], w[2]}});
        break;
      default:
        break;
    }
    i += wc;
  }
  for (auto& v : vars) {
    uint32_t id = v.first, ptr = v.second.first, sc = v.second.second;
    auto nm = names.find(id);
    std::string name = nm != names.end() ? nm->second : std::string();
    if (binding.count(id) || descSet.count(id)) {
      SpirvInfo::Binding b;
      b.set = descSet.count(id) ? descSet[id] : 0;
      b.binding = binding.count(id) ? binding[id] : 0;
      b.name = name;
      auto pt = ptrPointee.find(ptr);
      if (pt != ptrPointee.end()) {
        auto tn = names.find(pt->second);
        if (tn != names.end()) b.typeName = tn->second;
      }
      out.bindings.push_back(b);
    }
    if (sc == 1 && location.count(id)) out.inputs.push_back({location[id], name});  // Input
  }
  out.valid = true;
  return out;
}
}  // namespace feverscaler
