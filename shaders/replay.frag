#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Writes object motion only where the replayed surface is the visible one: its depth must match
// the game's depth buffer (same vertex math, so a small tolerance covers compiler differences).
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(location = 0) in vec4 currClip;
layout(location = 1) in vec4 prevClip;
layout(location = 0) out vec2 outMv;

void main() {
  float scene = viewDistance(texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r);
  float mine = viewDistance(gl_FragCoord.z);
  if (abs(mine - scene) > max(0.05, 0.004 * scene)) discard;
  outMv = (prevClip.xy / prevClip.w - currClip.xy / currClip.w) * 0.5;  // previous UV - current UV
}
