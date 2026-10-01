// SPIR-V patch: hashed alpha testing (see spirv_patch.h).
//
// Injected at the start of the entry point (uniform control flow, so derivatives are defined):
//   p     = posAmbient.xyz
//   scale = 4 / max(max(length(dFdx(p)), length(dFdy(p))), 1e-6)     // quarter-pixel cells
//   h     = fract(sin(dot(mod(floor(p * scale), 4096), K)) * 43758.5453)
// and before every OpReturn of the entry point:
//   if (out0.a < h) discard;
#include "spirv_patch.h"

#include <cstring>
#include <string>
#include <unordered_map>

namespace feverscaler {
namespace {
enum : uint32_t {
  OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15, OpTypeBool = 20, OpTypeFloat = 22,
  OpTypeVector = 23, OpTypePointer = 32, OpConstant = 43, OpConstantComposite = 44, OpFunction = 54,
  OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpDecorate = 71, OpVectorShuffle = 79,
  OpCompositeConstruct = 80, OpCompositeExtract = 81, OpFMul = 133, OpFDiv = 136, OpFMod = 141, OpDot = 148,
  OpFOrdLessThan = 184, OpDPdx = 207, OpDPdy = 208, OpSelectionMerge = 247, OpLabel = 248,
  OpBranchConditional = 250, OpKill = 252, OpReturn = 253,
};
enum : uint32_t { GlslLength = 66, GlslFMax = 40, GlslFloor = 8, GlslSin = 13, GlslFract = 10 };
constexpr uint32_t kDecorationLocation = 30, kStorageInput = 1, kStorageOutput = 3, kModelFragment = 4;

std::string LiteralString(const uint32_t* w, uint32_t n) {
  std::string s;
  for (uint32_t i = 0; i < n; ++i)
    for (int b = 0; b < 4; ++b) {
      char c = (char)((w[i] >> (8 * b)) & 0xFF);
      if (!c) return s;
      s += c;
    }
  return s;
}
uint32_t FloatBits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
void Emit(std::vector<uint32_t>& out, uint32_t op, std::initializer_list<uint32_t> operands) {
  out.push_back(((uint32_t)(operands.size() + 1) << 16) | op);
  out.insert(out.end(), operands);
}
}  // namespace

std::vector<uint32_t> PatchHashedAlpha(const uint32_t* code, size_t sizeBytes) {
  size_t n = sizeBytes / 4;
  if (n < 5 || code[0] != 0x07230203) return {};
  uint32_t bound = code[3];

  uint32_t glsl = 0, entryFn = 0, floatT = 0, boolT = 0, posVar = 0, outVar = 0;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vecT;  // id -> (component type, count)
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> ptrT;  // id -> (storage, pointee)
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vars;  // id -> (pointer type, storage)
  std::unordered_map<uint32_t, uint32_t> location;
  std::vector<uint32_t> posNamed;  // every id named posAmbient (locals and parameters share the name)
  size_t firstFunction = 0;
  size_t mainStart = 0, mainEnd = 0;
  for (size_t i = 5; i < n;) {
    uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
    if (!wc || i + wc > n) return {};
    const uint32_t* w = code + i;
    switch (op) {
      case OpName:
        if (LiteralString(w + 2, wc - 2) == "posAmbient") posNamed.push_back(w[1]);
        break;
      case OpExtInstImport:
        if (LiteralString(w + 2, wc - 2) == "GLSL.std.450") glsl = w[1];
        break;
      case OpEntryPoint:
        if (w[1] == kModelFragment) entryFn = w[2];
        break;
      case OpDecorate:
        if (wc >= 4 && w[2] == kDecorationLocation) location[w[1]] = w[3];
        break;
      case OpTypeBool: boolT = w[1]; break;
      case OpTypeFloat:
        if (w[2] == 32) floatT = w[1];
        break;
      case OpTypeVector: vecT[w[1]] = {w[2], w[3]}; break;
      case OpTypePointer: ptrT[w[1]] = {w[2], w[3]}; break;
      case OpVariable: vars[w[2]] = {w[1], w[3]}; break;
      case OpFunction:
        if (!firstFunction) firstFunction = i;
        if (w[2] == entryFn) mainStart = i;
        break;
      case OpFunctionEnd:
        if (mainStart && !mainEnd) mainEnd = i;
        break;
    }
    i += wc;
  }
  if (!glsl || !entryFn || !floatT || !mainStart || !mainEnd || !firstFunction) return {};
  auto vecOf = [&](uint32_t count) -> uint32_t {
    for (auto& kv : vecT)
      if (kv.second.first == floatT && kv.second.second == count) return kv.first;
    return 0;
  };
  uint32_t v3 = vecOf(3), v4 = vecOf(4);
  if (!v4) return {};
  // Inputs/outputs: posAmbient (vec4 input) and the vec4 output at location 0.
  for (uint32_t cand : posNamed) {
    auto v = vars.find(cand);
    if (v == vars.end()) continue;
    auto p = ptrT.find(v->second.first);
    if (v->second.second == kStorageInput && p != ptrT.end() && p->second.second == v4) posVar = cand;
  }
  for (auto& kv : vars) {
    auto p = ptrT.find(kv.second.first);
    if (kv.second.second == kStorageOutput && p != ptrT.end() && p->second.second == v4 && location.count(kv.first) &&
        location[kv.first] == 0)
      outVar = kv.first;
  }
  if (!posVar || !outVar) return {};

  // ---- new types and constants (before the first function)
  std::vector<uint32_t> decl;
  auto id = [&]() { return bound++; };
  if (!v3) {
    v3 = id();
    Emit(decl, OpTypeVector, {v3, floatT, 3});
  }
  if (!boolT) {
    boolT = id();
    Emit(decl, OpTypeBool, {boolT});
  }
  uint32_t cQuarter = id(), cTiny = id(), c4096 = id(), c4096v = id(), kx = id(), ky = id(), kz = id(), kv = id(),
           cMul = id();
  Emit(decl, OpConstant, {floatT, cQuarter, FloatBits(4.0f)});
  Emit(decl, OpConstant, {floatT, cTiny, FloatBits(1e-6f)});
  Emit(decl, OpConstant, {floatT, c4096, FloatBits(4096.0f)});
  Emit(decl, OpConstantComposite, {v3, c4096v, c4096, c4096, c4096});
  Emit(decl, OpConstant, {floatT, kx, FloatBits(12.9898f)});
  Emit(decl, OpConstant, {floatT, ky, FloatBits(78.233f)});
  Emit(decl, OpConstant, {floatT, kz, FloatBits(37.719f)});
  Emit(decl, OpConstantComposite, {v3, kv, kx, ky, kz});
  Emit(decl, OpConstant, {floatT, cMul, FloatBits(43758.5453f)});

  // ---- the hash, at the start of main's first block (after its OpLabel and OpVariables)
  size_t insertAt = 0;
  for (size_t i = mainStart; i < mainEnd;) {
    uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
    if (op == OpLabel && !insertAt) {
      size_t j = i + wc;
      while (j < mainEnd && (code[j] & 0xFFFF) == OpVariable) j += code[j] >> 16;
      insertAt = j;
      break;
    }
    i += wc;
  }
  if (!insertAt) return {};
  std::vector<uint32_t> hash;
  uint32_t pos = id(), p3 = id(), dx = id(), dy = id(), lx = id(), ly = id(), m = id(), m2 = id(), sc = id(),
           scv = id(), ps = id(), fl = id(), md = id(), d = id(), sn = id(), h1 = id(), h = id();
  Emit(hash, OpLoad, {v4, pos, posVar});
  Emit(hash, OpVectorShuffle, {v3, p3, pos, pos, 0, 1, 2});
  Emit(hash, OpDPdx, {v3, dx, p3});
  Emit(hash, OpDPdy, {v3, dy, p3});
  Emit(hash, OpExtInst, {floatT, lx, glsl, GlslLength, dx});
  Emit(hash, OpExtInst, {floatT, ly, glsl, GlslLength, dy});
  Emit(hash, OpExtInst, {floatT, m, glsl, GlslFMax, lx, ly});
  Emit(hash, OpExtInst, {floatT, m2, glsl, GlslFMax, m, cTiny});
  Emit(hash, OpFDiv, {floatT, sc, cQuarter, m2});
  Emit(hash, OpCompositeConstruct, {v3, scv, sc, sc, sc});
  Emit(hash, OpFMul, {v3, ps, p3, scv});
  Emit(hash, OpExtInst, {v3, fl, glsl, GlslFloor, ps});
  Emit(hash, OpFMod, {v3, md, fl, c4096v});
  Emit(hash, OpDot, {floatT, d, md, kv});
  Emit(hash, OpExtInst, {floatT, sn, glsl, GlslSin, d});
  Emit(hash, OpFMul, {floatT, h1, sn, cMul});
  Emit(hash, OpExtInst, {floatT, h, glsl, GlslFract, h1});

  // ---- rebuild the module
  std::vector<uint32_t> out;
  out.reserve(n + decl.size() + hash.size() + 64);
  out.insert(out.end(), code, code + 5);
  for (size_t i = 5; i < n;) {
    uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
    if (i == firstFunction) out.insert(out.end(), decl.begin(), decl.end());
    if (i == insertAt) out.insert(out.end(), hash.begin(), hash.end());
    if (op == OpEntryPoint && code[i + 2] == entryFn) {
      // The interface must list every input the entry point reads (posAmbient may be unused so far).
      bool listed = false;
      uint32_t nameWords = 0;
      while (!nameWords || (code[i + 2 + nameWords] >> 24) != 0) ++nameWords;  // the entry point name literal
      for (uint32_t k = 3 + nameWords; k < wc; ++k) listed |= code[i + k] == posVar;
      if (!listed) {
        out.push_back(((wc + 1) << 16) | op);
        out.insert(out.end(), code + i + 1, code + i + wc);
        out.push_back(posVar);
        i += wc;
        continue;
      }
    }
    if (op == OpReturn && i > mainStart && i < mainEnd) {
      // if (out0.a < h) discard; return;
      uint32_t o = id(), a = id(), c = id(), kill = id(), merge = id();
      Emit(out, OpLoad, {v4, o, outVar});
      Emit(out, OpCompositeExtract, {floatT, a, o, 3});
      Emit(out, OpFOrdLessThan, {boolT, c, a, h});
      Emit(out, OpSelectionMerge, {merge, 0});
      Emit(out, OpBranchConditional, {c, kill, merge});
      Emit(out, OpLabel, {kill});
      Emit(out, OpKill, {});
      Emit(out, OpLabel, {merge});
    }
    out.insert(out.end(), code + i, code + i + wc);
    i += wc;
  }
  out[3] = bound;
  return out;
}
}  // namespace feverscaler

namespace feverscaler {
std::vector<uint32_t> PatchSsr(const uint32_t* code, size_t sizeBytes) {
  // Original (ssr_apply.fs):
  //   float stride = u_ssao.stride;
  //   float jitterFraction = 1.0 + float((int(fc.x) + int(fc.y)) & 1) * 0.5;
  // Patched, while the view is jittered (DLSS: u_view.proj[2][0] or [2][1] != 0):
  //   stride *= max(u_ssao.normalMapRes1.y / kSsrReferenceHeight, 1.0)
  //   jitterFraction = 1.0 + fract(jy + 0.5 + bayer2x2 / 4), jy = this frame's DLSS jitter
  enum : uint32_t {
    OpMemberName = 6, OpTypeInt = 21, OpAccessChain = 65, OpStore = 62, OpIAdd = 128, OpFAdd = 129,
    OpConvertFToS = 110, OpConvertSToF = 111, OpSelect = 169, OpBitwiseAnd = 199, OpFOrdNotEqual = 182,
    OpLogicalOr = 166, OpBitwiseXor = 198,
  };
  constexpr uint32_t kStorageUniform = 2;
  constexpr float kSsrReferenceHeight = 480.0f;
  size_t n = sizeBytes / 4;
  if (n < 5 || code[0] != 0x07230203) return {};
  uint32_t bound = code[3];
  uint32_t glsl = 0, intT = 0, floatT = 0, boolT = 0, ptrUniformFloat = 0;
  std::vector<uint32_t> viewNamed, ssaoNamed;
  std::unordered_map<uint32_t, bool> int32Types;                       // OpTypeInt 32 (either signedness)
  std::unordered_map<uint32_t, uint32_t> intConst;                     // signed int value -> id
  std::unordered_map<uint32_t, uint32_t> constVal;                     // 32-bit int constant id -> value
  std::unordered_map<uint32_t, uint32_t> producer;                     // result id -> opcode
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> args;    // result id -> first two operands
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> ptrT;    // id -> (storage, pointee)
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vars;    // id -> (pointer type, storage)
  std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, std::string>>> memberNames;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> chain1;  // access chain id -> (base, index id), one index
  std::vector<std::pair<size_t, uint32_t>> floatLoads;                 // (position, result) of float loads through chain1
  size_t firstFunction = 0, cvtPos = 0, usePos = 0;
  uint32_t andResult = 0, cvtResult = 0, xInt = 0, yInt = 0;  // xInt / yInt: int(gl_FragCoord.x / .y)
  for (size_t i = 5; i < n;) {
    uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
    if (!wc || i + wc > n) return {};
    const uint32_t* w = code + i;
    switch (op) {
      case OpName: {
        std::string nm = LiteralString(w + 2, wc - 2);
        if (nm == "u_view") viewNamed.push_back(w[1]);
        if (nm == "u_ssao") ssaoNamed.push_back(w[1]);
        break;
      }
      case OpMemberName: memberNames[w[1]].push_back({w[2], LiteralString(w + 3, wc - 3)}); break;
      case OpExtInstImport:
        if (LiteralString(w + 2, wc - 2) == "GLSL.std.450") glsl = w[1];
        break;
      case OpTypeInt:
        if (w[2] == 32) int32Types[w[1]] = true;
        if (w[2] == 32 && w[3] == 1) intT = w[1];
        break;
      case OpTypeFloat:
        if (w[2] == 32) floatT = w[1];
        break;
      case OpTypeBool: boolT = w[1]; break;
      case OpTypePointer: ptrT[w[1]] = {w[2], w[3]}; break;
      case OpConstant:
        if (wc == 4 && int32Types.count(w[1])) constVal[w[2]] = w[3];
        if (w[1] == intT && wc == 4) intConst.emplace(w[3], w[2]);
        break;
      case OpVariable: vars[w[2]] = {w[1], w[3]}; break;
      case OpFunction:
        if (!firstFunction) firstFunction = i;
        break;
      case OpAccessChain:
        if (wc == 5) chain1[w[2]] = {w[3], w[4]};
        break;
      case OpLoad:
        if (w[1] == floatT && chain1.count(w[3])) floatLoads.push_back({i, w[2]});
        break;
      default: break;
    }
    if (op == OpIAdd || op == OpConvertFToS || op == OpBitwiseAnd) {
      producer[w[2]] = op;
      args[w[2]] = {w[3], wc > 4 ? w[4] : 0};
    }
    if (op == OpBitwiseAnd && wc == 5 && !andResult) {
      uint32_t one = intConst.count(1) ? intConst[1] : 0;
      uint32_t sum = (w[4] == one) ? w[3] : (w[3] == one ? w[4] : 0);
      if (one && sum && producer.count(sum) && producer[sum] == OpIAdd) {
        auto ops = args[sum];
        if (producer.count(ops.first) && producer[ops.first] == OpConvertFToS && producer.count(ops.second) &&
            producer[ops.second] == OpConvertFToS)
          andResult = w[2];
          xInt = ops.first;
          yInt = ops.second;
      }
    }
    if (op == OpConvertSToF && andResult && !cvtPos && w[3] == andResult) {
      cvtPos = i;
      cvtResult = w[2];
    }
    if ((op == OpFMul || op == OpFAdd) && cvtResult && !usePos && i > cvtPos && (w[3] == cvtResult || w[4] == cvtResult))
      usePos = i;
    i += wc;
  }
  if (!cvtPos || !usePos || !glsl || !intT || !floatT || !firstFunction) return {};
  // The uniform blocks and their members.
  auto block = [&](const std::vector<uint32_t>& named, uint32_t* var, uint32_t* type) {
    for (uint32_t cand : named) {
      auto v = vars.find(cand);
      if (v == vars.end() || v->second.second != kStorageUniform) continue;
      auto p = ptrT.find(v->second.first);
      if (p == ptrT.end()) continue;
      *var = cand;
      *type = p->second.second;
    }
    return *var != 0;
  };
  auto member = [&](uint32_t type, const char* name) {
    for (auto& mn : memberNames[type])
      if (mn.second == name) return mn.first;
    return ~0u;
  };
  uint32_t viewVar = 0, viewStruct = 0, ssaoVar = 0, ssaoStruct = 0;
  if (!block(viewNamed, &viewVar, &viewStruct) || !block(ssaoNamed, &ssaoVar, &ssaoStruct)) return {};
  uint32_t projIdx = member(viewStruct, "proj"), strideIdx = member(ssaoStruct, "stride"),
           resIdx = member(ssaoStruct, "normalMapRes1");
  if (projIdx == ~0u || strideIdx == ~0u || resIdx == ~0u) return {};
  // `float stride = u_ssao.stride;`: the load, and the store of its value.
  size_t strideLoadPos = 0, strideStorePos = 0;
  uint32_t strideLoad = 0;
  for (auto& ld : floatLoads) {
    auto c = chain1[code[ld.first + 3]];
    if (c.first == ssaoVar && constVal.count(c.second) && constVal[c.second] == strideIdx) {
      strideLoadPos = ld.first;
      strideLoad = ld.second;
      break;
    }
  }
  if (!strideLoadPos) return {};
  for (size_t i = strideLoadPos; i < n;) {
    uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
    if (op == OpStore && wc == 3 && code[i + 2] == strideLoad) {
      strideStorePos = i;
      break;
    }
    if (op == OpFunctionEnd) break;
    i += wc;
  }
  if (!strideStorePos) return {};
  for (auto& kv : ptrT)
    if (kv.second.first == kStorageUniform && kv.second.second == floatT) ptrUniformFloat = kv.first;

  std::vector<uint32_t> decl;
  auto id = [&]() { return bound++; };
  if (!boolT) {
    boolT = id();
    Emit(decl, OpTypeBool, {boolT});
  }
  if (!ptrUniformFloat) {
    ptrUniformFloat = id();
    Emit(decl, OpTypePointer, {ptrUniformFloat, kStorageUniform, floatT});
  }
  auto intC = [&](uint32_t v) {
    uint32_t c = id();
    Emit(decl, OpConstant, {intT, c, v});
    return c;
  };
  auto floatC = [&](float v) {
    uint32_t c = id();
    Emit(decl, OpConstant, {floatT, c, FloatBits(v)});
    return c;
  };
  uint32_t c0 = intC(0), c1 = intC(1), c2 = intC(2), cProj = intC(projIdx), cRes = intC(resIdx);
  uint32_t f0 = floatC(0.0f), f1 = floatC(1.0f), fRef = floatC(kSsrReferenceHeight), fHalf = floatC(0.5f),
           fTwo = floatC(2.0f), fQuarter = floatC(0.25f);

  // jittered = u_view.proj[2][0] != 0 || u_view.proj[2][1] != 0  (DLSS running); *p21 = proj[2][1]
  auto jittered = [&](std::vector<uint32_t>& out, uint32_t* p21 = nullptr) {
    uint32_t pp0 = id(), p0 = id(), pp1 = id(), p1 = id(), n0 = id(), n1 = id(), j = id();
    Emit(out, OpAccessChain, {ptrUniformFloat, pp0, viewVar, cProj, c2, c0});
    Emit(out, OpLoad, {floatT, p0, pp0});
    Emit(out, OpAccessChain, {ptrUniformFloat, pp1, viewVar, cProj, c2, c1});
    Emit(out, OpLoad, {floatT, p1, pp1});
    Emit(out, OpFOrdNotEqual, {boolT, n0, p0, f0});
    Emit(out, OpFOrdNotEqual, {boolT, n1, p1, f0});
    Emit(out, OpLogicalOr, {boolT, j, n0, n1});
    if (p21) *p21 = p1;
    return j;
  };

  std::vector<uint32_t> out;
  out.reserve(n + decl.size() + 96);
  out.insert(out.end(), code, code + 5);
  uint32_t strideSel = 0, jitterSel = 0;
  for (size_t i = 5; i < n;) {
    uint32_t wc = code[i] >> 16;
    if (i == firstFunction) out.insert(out.end(), decl.begin(), decl.end());
    if (i == strideStorePos || i == usePos) {
      std::vector<uint32_t> inst(code + i, code + i + wc);
      bool use = i == usePos;
      uint32_t from = use ? cvtResult : strideLoad, to = use ? jitterSel : strideSel;
      for (size_t k = use ? 3 : 2; k < inst.size(); ++k)
        if (inst[k] == from) inst[k] = to;
      out.insert(out.end(), inst.begin(), inst.end());
      i += wc;
      continue;
    }
    out.insert(out.end(), code + i, code + i + wc);
    if (i == strideLoadPos) {
      // Reach independent of the render resolution: the ray takes u_ssao.maxSteps steps of `stride`
      // pixels, so at a higher render height it covered less of the screen (cut-off reflections).
      uint32_t j = jittered(out), pr = id(), resY = id(), q = id(), k = id(), s2 = id();
      strideSel = id();
      Emit(out, OpAccessChain, {ptrUniformFloat, pr, ssaoVar, cRes, c1});
      Emit(out, OpLoad, {floatT, resY, pr});
      Emit(out, OpFDiv, {floatT, q, resY, fRef});
      Emit(out, OpExtInst, {floatT, k, glsl, GlslFMax, q, f1});
      Emit(out, OpFMul, {floatT, s2, strideLoad, k});
      Emit(out, OpSelect, {floatT, strideSel, j, s2, strideLoad});
    }
    if (i == cvtPos) {
      // Ray offset = fract(frame phase + bayer / 4): the four pixels of every 2x2 block step at 0,
      // 1/4, 1/2 and 3/4 of the stride (ordered dither, bayer = [[0, 2], [3, 1]]), rotated every
      // frame by the DLSS jitter (proj[2][1] = 2 jy / H, jy in [-0.5, 0.5) on a Halton sequence).
      // Every small neighbourhood always holds all four offsets, so its average is stable and DLSS
      // resolves the pixel-level variation (like the hashed alpha on foliage); a fixed pattern (the
      // game's checkerboard) would stay as a pattern, a shift shared by all pixels would flicker.
      uint32_t p21 = 0, j = jittered(out, &p21), pr = id(), resY = id(), t = id(), t2 = id(), t3 = id(), ph = id(),
               ph2 = id(), xy = id(), a = id(), a2 = id(), yb = id(), b = id(), bf = id(), bq = id(), s = id();
      jitterSel = id();
      Emit(out, OpAccessChain, {ptrUniformFloat, pr, ssaoVar, cRes, c1});
      Emit(out, OpLoad, {floatT, resY, pr});
      Emit(out, OpFMul, {floatT, t, p21, resY});
      Emit(out, OpFMul, {floatT, t2, t, fHalf});
      Emit(out, OpFAdd, {floatT, t3, t2, fHalf});
      Emit(out, OpBitwiseXor, {intT, xy, xInt, yInt});
      Emit(out, OpBitwiseAnd, {intT, a, xy, c1});
      Emit(out, OpIAdd, {intT, a2, a, a});
      Emit(out, OpBitwiseAnd, {intT, yb, yInt, c1});
      Emit(out, OpIAdd, {intT, b, a2, yb});
      Emit(out, OpConvertSToF, {floatT, bf, b});
      Emit(out, OpFMul, {floatT, bq, bf, fQuarter});
      Emit(out, OpFAdd, {floatT, s, t3, bq});
      Emit(out, OpExtInst, {floatT, ph, glsl, GlslFract, s});
      Emit(out, OpFMul, {floatT, ph2, ph, fTwo});  // the original multiplies this term by 0.5
      Emit(out, OpSelect, {floatT, jitterSel, j, ph2, cvtResult});
    }
    i += wc;
  }
  out[3] = bound;
  return out;
}
}  // namespace feverscaler
