#pragma once
#include <sl.h>
#include <sl_consts.h>

namespace feverscaler {
// Column-major 4x4 (GLSL layout: m[col*4 + row]), column-vector convention (v' = M v).
struct DMat4 {
  double m[16];
  static DMat4 Identity();
  static DMat4 FromFloat(const float* colMajor);
  double& at(int row, int col) { return m[col * 4 + row]; }
  double at(int row, int col) const { return m[col * 4 + row]; }
};
DMat4 Mul(const DMat4& a, const DMat4& b);
DMat4 Inverse(const DMat4& a);
// GL clip (as the game computes it) -> the clip space the game actually rasterises with:
// the vertex shaders apply z = 0.5z + 0.5w (Vulkan depth range) and y = -y.
DMat4 GlToVkClip();
void ToFloat(const DMat4& a, float* colMajor16);
// Streamline matrices are row-major with row vectors (v' = v M), i.e. the transpose of ours.
sl::float4x4 ToSl(const DMat4& a);
}  // namespace feverscaler
