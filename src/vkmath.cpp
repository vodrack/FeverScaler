#include "vkmath.h"

#include <cmath>
#include <utility>

namespace feverscaler {
DMat4 DMat4::Identity() {
  DMat4 r{};
  for (int i = 0; i < 4; ++i) r.m[i * 4 + i] = 1.0;
  return r;
}
DMat4 DMat4::FromFloat(const float* c) {
  DMat4 r;
  for (int i = 0; i < 16; ++i) r.m[i] = c[i];
  return r;
}
DMat4 Mul(const DMat4& a, const DMat4& b) {
  DMat4 r{};
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a.at(row, k) * b.at(k, col);
      r.at(row, col) = s;
    }
  return r;
}
DMat4 Inverse(const DMat4& a) {
  // Gauss-Jordan with partial pivoting on a row-major copy.
  double w[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      w[r][c] = a.at(r, c);
      w[r][c + 4] = (r == c) ? 1.0 : 0.0;
    }
  for (int c = 0; c < 4; ++c) {
    int piv = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(w[r][c]) > std::fabs(w[piv][c])) piv = r;
    if (piv != c)
      for (int k = 0; k < 8; ++k) std::swap(w[c][k], w[piv][k]);
    double d = w[c][c];
    if (std::fabs(d) < 1e-300) return DMat4::Identity();
    for (int k = 0; k < 8; ++k) w[c][k] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double f = w[r][c];
      if (f == 0) continue;
      for (int k = 0; k < 8; ++k) w[r][k] -= f * w[c][k];
    }
  }
  DMat4 out;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out.at(r, c) = w[r][c + 4];
  return out;
}
DMat4 GlToVkClip() {
  DMat4 f = DMat4::Identity();
  f.at(1, 1) = -1.0;  // y = -y
  f.at(2, 2) = 0.5;   // z = 0.5 z + 0.5 w
  f.at(2, 3) = 0.5;
  return f;
}
void ToFloat(const DMat4& a, float* c) {
  for (int i = 0; i < 16; ++i) c[i] = (float)a.m[i];
}
sl::float4x4 ToSl(const DMat4& a) {
  // Transposing a column-major matrix into row-major storage keeps the memory order: row i of
  // the Streamline matrix is column i of ours.
  sl::float4x4 r;
  for (int i = 0; i < 4; ++i)
    r.setRow(i, sl::float4((float)a.m[i * 4 + 0], (float)a.m[i * 4 + 1], (float)a.m[i * 4 + 2], (float)a.m[i * 4 + 3]));
  return r;
}
}  // namespace feverscaler
