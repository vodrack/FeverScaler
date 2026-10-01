#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Re-draws a skinned mesh with this frame's and last frame's joint matrices (world space).
// Mirrors mat/vs/skinning/util.vs + normal.vs: skinMat = sum(joints[int(i)] * 1.001001 * fract(i)).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inInfluence;
layout(set = 0, binding = 0, std430) readonly buffer Mats { mat4 m[]; } mats;
layout(push_constant) uniform PC {
  uint base;   // first current joint matrix
  uint count;  // joints per block; previous block follows at base + count
} pc;
layout(location = 0) out vec4 currClip;
layout(location = 1) out vec4 prevClip;

const float WEIGHTS_SUM_1 = 1.0 / 0.999;

mat4 skinMatrix(uint base, vec4 infl) {
  mat4 r = mats.m[base + uint(int(infl[0]))] * (WEIGHTS_SUM_1 * fract(infl[0]));
  for (int i = 1; i < 4; ++i) r += mats.m[base + uint(int(infl[i]))] * (WEIGHTS_SUM_1 * fract(infl[i]));
  return r;
}

void main() {
  vec4 p = vec4(inPos, 1.0);
  vec4 wc = skinMatrix(pc.base, inInfluence) * p;
  vec4 wp = skinMatrix(pc.base + pc.count, inInfluence) * p;
  vec4 cc = toRasterClip(u_frame.currVP * vec4(wc.xyz, 1.0));
  vec4 cp = toRasterClip(u_frame.prevVP * vec4(wp.xyz, 1.0));
  currClip = cc;
  prevClip = cp;
  gl_Position = toRasterClip(u_frame.currVPJit * vec4(wc.xyz, 1.0));  // lands on the game's (jittered) depth
}
