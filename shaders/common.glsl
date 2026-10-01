// Shared by all feverscaler shaders. One descriptor set layout for everything:
//   b0 SSBO  instance / joint matrices (current block followed by previous block)
//   b1       our copy of the scene depth (depth aspect)
//   b2 UBO   per-frame camera data
//   b3       motion-vector image (compute pass writes camera motion here)
layout(set = 0, binding = 2, std140) uniform Frame {
  mat4 currVP;          // game's u_view.projView this frame (OpenGL clip), without DLSS jitter
  mat4 prevVP;          // ... last frame
  mat4 clipToPrevClip;  // jittered rasterised clip (Vulkan: y down, z 0..1) -> previous rasterised clip
  vec4 size;            // render width, height, 1/width, 1/height
  vec4 nearFar;         // near, far
  mat4 currVPJit;       // currVP with the DLSS jitter the game rendered with (= currVP without DLSS)
  vec4 jitter;          // xy: jitter as an offset in rasterised NDC
} u_frame;

// The game's vertex shaders end with: z = 0.5 z + 0.5 w; y = -y.
vec4 toRasterClip(vec4 c) {
  c.z = 0.5 * c.z + 0.5 * c.w;
  c.y = -c.y;
  return c;
}

// Depth buffer value -> distance along the view axis (same formula as the game's SSAO/SSR).
float viewDistance(float d) {
  float n = u_frame.nearFar.x, f = u_frame.nearFar.y;
  return 2.0 * f * n / ((f + n) - (2.0 * d - 1.0) * (f - n));
}
