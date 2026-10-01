#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Re-draws a moving instanced mesh with its current and previous model matrices.
// Mirrors mat/vs/std/normal.vs: posAmbient = instAttrModel * attrPosition;
// gl_Position = u_view.projView * vec4(posAmbient.xyz, 1.0) (+ z remap, y flip).
layout(location = 0) in vec3 inPos;
layout(set = 0, binding = 0, std430) readonly buffer Mats { mat4 m[]; } mats;
layout(push_constant) uniform PC {
  uint base;   // first current matrix in the SSBO
  uint count;  // instances; previous matrices follow at base + count
} pc;
layout(location = 0) out vec4 currClip;
layout(location = 1) out vec4 prevClip;

void main() {
  vec4 p = vec4(inPos, 1.0);
  vec4 wc = mats.m[pc.base + uint(gl_InstanceIndex)] * p;
  vec4 wp = mats.m[pc.base + pc.count + uint(gl_InstanceIndex)] * p;
  vec4 cc = toRasterClip(u_frame.currVP * vec4(wc.xyz, 1.0));
  vec4 cp = toRasterClip(u_frame.prevVP * vec4(wp.xyz, 1.0));
  currClip = cc;
  prevClip = cp;
  gl_Position = toRasterClip(u_frame.currVPJit * vec4(wc.xyz, 1.0));  // lands on the game's (jittered) depth
}
