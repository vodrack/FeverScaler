// Per-frame orchestration: builds the DLSS-G inputs and drives the frame-generation gate.
//
//   game submits ... -> (per submit) read camera + instance/joint matrices from mapped memory
//   submit containing the UI pass -> our pre-pass submit first:
//        copy scene depth, camera motion (compute), replay moving objects (graphics),
//        then tag depth / motion vectors / HUD-less and set camera constants
//   UI pass begin (recorded in the game's command buffer) -> blit scene colour to HUD-less image
//   present -> gate + markers + Streamline present (DLSS-G generates here)
//
// DLSS Super Resolution (when on): the main camera block is jittered in mapped memory at the
// first submit that uses it; at the UI pass DLSS upscales the post-compose scene copy into our
// display-size image, and the UI pass samples that instead of the game's own scene texture
// (descriptor writes are redirected in the tracker).
#include "frame.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "bridge.h"
#include "config.h"
#include "game_scale.h"
#include "game_settings.h"
#include "gpu.h"
#include "hooks.h"
#include "log.h"
#include "diagnostics.h"
#include "mip_bias.h"
#include "motion_match.h"
#include "overlay.h"
#include "sl_bridge.h"
#include "sr_policy.h"
#include "tracker.h"
#include "vram.h"
#include "vkmath.h"

namespace feverscaler {
struct Camera {
  bool valid = false;
  float projView[16], proj[16], view[16], viewInv[16];
  float camPos[3];
  float nearZ = 0, farZ = 0, fov = 0;
  int width = 0, height = 0;
};
struct Replay {
  DrawRec rec;
  uint32_t base = 0, count = 0;
  uint32_t order = 0;
};

static std::mutex g_mx;  // frame state
static std::atomic<uint64_t> g_frame{0};
static bool g_frameOpen = false;
// Submits with the world's main pass since the frame began. Normally one; many more mean the game
// renders without presenting (minimised window), and the frame's data is started over.
static uint32_t g_mainPassesOpen = 0;
constexpr uint32_t kMaxMainPassesPerFrame = 8;
constexpr size_t kMaxFrameMatrixFloats = size_t(32) << 20;  // 128 MB of matrices in one frame
static uint32_t g_slot = 0;
static Camera g_cam, g_prevCam;
static bool g_haveMain = false;
static VkImage g_mainDepth = VK_NULL_HANDLE;
static VkFormat g_mainDepthFmt = VK_FORMAT_UNDEFINED;
static VkSampleCountFlagBits g_mainSamples = VkSampleCountFlagBits(0);
static uint32_t g_mainW = 0, g_mainH = 0, g_prevW = 0, g_prevH = 0;
static std::vector<Replay> g_replays;
static uint32_t g_replayMesh = 0, g_replaySkinned = 0;
static uint32_t g_motionDrawOrder = 0;
static bool g_rigidReplaysBuilt = false;
// This frame's and last frame's matrices per draw key, in two flat arenas that swap roles each
// frame (no per-draw allocations once warmed up).
struct MatRange {
  uint32_t off, n;  // offset in floats, number of mat4
};
struct RigidDraw {
  DrawRec rec;
  MatRange matrices;
  uint32_t order;
};
struct MatStore {
  std::vector<float> arena;
  std::unordered_map<uint64_t, std::vector<RigidDraw>> rigid; // all instances of the same geometry
  // Skinned meshes: mesh key -> every joint block drawn with that mesh this frame. Matched to last
  // frame by position, not by draw order (the game reorders and culls these draws as the camera
  // moves; pairing by order put one tree's joints against another's).
  std::unordered_map<uint64_t, std::vector<MatRange>> skinned;
  void Clear() {
    arena.clear();
    for (auto it = rigid.begin(); it != rigid.end();) {
      if (it->second.empty()) it = rigid.erase(it);
      else { it->second.clear(); ++it; }
    }
    for (auto& kv : skinned) kv.second.clear();
  }
};
static MatStore g_store[2];
static int g_cur = 0;  // g_store[g_cur] = this frame, g_store[g_cur ^ 1] = previous frame
static bool g_tagged = false;
static std::atomic<long long> g_cpuProcessTicks{0}, g_cpuPrePassTicks{0};  // plugin CPU time (QPC ticks)
// gate and frame-generation switches (present thread)
static bool g_genOn = false, g_userOff = false, g_lastValid = false;
static uint32_t g_validStreak = 0, g_idleStreak = 0, g_genFrames = 1;
static float g_baseFps = 0;

// ---- DLSS Super Resolution state -------------------------------------------------------------------
// g_srOn and g_dlssLive change only at present, so every frame is consistently jittered or not.
static std::atomic<bool> g_srOn{false};
static bool g_srWanted = false;                // present thread
static std::atomic<bool> g_dlssLive{false};    // DLSS made the last frames' image: jitter the camera
static std::atomic<bool> g_dlssFrameOk{false}; // DLSS evaluated in this frame's command buffer
static int g_dlssMisses = 0;
static std::atomic<bool> g_srResetNext{true};  // DLSS history reset at the next evaluate
static int g_redirLinger = 0;                  // UI passes that must still fill our image after SR off
static std::atomic<uint32_t> g_jitterW{0}, g_jitterH{0};  // render size the jitter is computed for
static std::atomic<bool> g_srFailed{false};
static uint32_t g_srRenderW = 0, g_srRenderH = 0, g_srOutW = 0, g_srOutH = 0;  // last evaluate (menu)
static SrScalePolicy g_srScale;                // present thread

bool SrRequested() { return Cfg().superResolution && SlDlssAvailable() && !g_srFailed.load(); }
static bool RenderWorkRequested() {
  return SlCoreReady() && ((Cfg().frameGeneration && SlFgReady()) || SrRequested());
}

struct CameraPatch {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceSize off = 0;
  uint64_t frame = ~0ull;
  float orig[64], patched[64];  // projView, projViewInverse, proj, projInverse
};
static std::vector<CameraPatch> g_patches;  // camera blocks we wrote into (recent ones)
static uint64_t g_camFrame = ~0ull;         // frame whose camera g_cam holds

static float Halton(uint32_t i, uint32_t b) {
  float f = 1.0f, r = 0.0f;
  while (i) {
    f /= (float)b;
    r += f * (float)(i % b);
    i /= b;
  }
  return r;
}
// This frame's sub-pixel jitter in render pixels (Halton 2,3; at least 8 * scale^2 phases), or
// false when DLSS is not running. A pure function of the frame number: the camera patch (at submit)
// and the DLSS constants (at command recording) agree even when the game records its last command
// buffer before submitting its 3D passes.
static bool FrameJitter(float* jx, float* jy) {
  uint32_t rw = g_jitterW.load();
  if (!g_srOn.load() || !g_dlssLive.load() || !rw) {
    *jx = *jy = 0;
    return false;
  }
  uint32_t ow = SwapchainExtent().width;
  float ratio = ow > rw ? (float)ow / (float)rw : 1.0f;
  uint32_t phases = std::max(8u, (uint32_t)std::ceil(8.0f * ratio * ratio));
  uint32_t i = (uint32_t)(g_frame.load() % phases) + 1;
  *jx = Halton(i, 2) - 0.5f;
  *jy = Halton(i, 3) - 0.5f;
  return true;
}
// OpenGL-convention clip jitter (column-major): content moves by (+jx, +jy) pixels, i.e.
// clip.x += dx * w and clip.y += dy * w with dx = 2 jx / W, dy = -2 jy / H (clip y points up).
static void JitterMat(float* m, float dx, float dy) {
  for (int c = 0; c < 4; ++c) {
    m[c * 4 + 0] += dx * m[c * 4 + 3];
    m[c * 4 + 1] += dy * m[c * 4 + 3];
  }
}
static void JitterInverse(float* n, float dx, float dy) {  // n * J^-1 only changes the last column
  for (int r = 0; r < 4; ++r) n[12 + r] -= dx * n[r] + dy * n[4 + r];
}

uint32_t RecordingSlot() { return (uint32_t)(g_frame.load() % kSlots); }

// ---- camera ---------------------------------------------------------------------------------------
// View UBO (std140): projView 0, projViewInverse 64, proj 128, projInverse 192, view 256,
// viewInverse 320, camPos 384, lodFactor 400, near 416, far 420, time 424, width 428, height 432,
// boundsMin 440, boundsMax 448, fov 456, mode 460.
static bool ParseCamera(const uint8_t* raw, Camera* out) {
  Camera c;
  memcpy(c.projView, raw + 0, 64);
  memcpy(c.proj, raw + 128, 64);
  memcpy(c.view, raw + 256, 64);
  memcpy(c.viewInv, raw + 320, 64);
  memcpy(c.camPos, raw + 384, 12);
  memcpy(&c.nearZ, raw + 416, 4);
  memcpy(&c.farZ, raw + 420, 4);
  memcpy(&c.width, raw + 428, 4);
  memcpy(&c.height, raw + 432, 4);
  memcpy(&c.fov, raw + 456, 4);
  // Perspective main camera only (shadow cascades use orthographic blocks of their own).
  if (std::fabs(c.proj[11] + 1.0f) > 1e-3f || std::fabs(c.proj[15]) > 1e-3f) return false;
  if (!(c.nearZ > 0 && c.farZ > c.nearZ)) return false;
  c.valid = true;
  *out = c;
  return true;
}

// Reads the main camera from a u_view block and, while DLSS runs, writes this frame's jittered
// matrices back into it (mapped memory, before the submit that makes the GPU read it). Every block
// holding the main camera gets the same jitter; other cameras (shadows, reflections) are left alone.
// mayDefine: the block can be the first main camera of the frame (main 3D pass); other blocks
// (post-processing) are only jittered when they match the camera already found.
static void CameraBlockSeen(VkDescriptorSet set0, bool mayDefine = true) {
  UboBinding ub;
  if (!GetUboBinding(set0, 10, &ub)) return;
  uint64_t frame = g_frame.load();
  CameraPatch* rec = nullptr;
  for (auto& p : g_patches)
    if (p.buf == ub.buffer && p.off == ub.offset) rec = &p;
  uint8_t raw[464];
  if (!ReadBufferBytes(ub.buffer, ub.offset, raw, sizeof(raw))) return;
  // Still exactly what we wrote last time: the game did not upload the camera again.
  if (rec && !memcmp(raw, rec->patched, 256)) memcpy(raw, rec->orig, 256);
  Camera c;
  if (!ParseCamera(raw, &c)) return;
  if (!g_cam.valid) {
    if (!mayDefine) return;
    g_cam = c;
    g_camFrame = frame;
  } else if (memcmp(c.projView, g_cam.projView, 64) != 0) return;  // not the main camera
  // Frame data can be reset after many submits without advancing the present counter. The
  // camera must still be defined again from the unjittered block; only the write is already done.
  if (rec && rec->frame == frame) return;
  float jx, jy;
  if (!FrameJitter(&jx, &jy) || !g_mainW || !g_mainH) return;
  float m[64];
  memcpy(m, raw, 256);
  float dx = 2.0f * jx / (float)g_mainW, dy = -2.0f * jy / (float)g_mainH;
  JitterMat(m + 0, dx, dy);
  JitterInverse(m + 16, dx, dy);
  JitterMat(m + 32, dx, dy);
  JitterInverse(m + 48, dx, dy);
  if (!WriteBufferBytes(ub.buffer, ub.offset, m, 256)) return;
  if (!rec) {
    if (g_patches.size() >= 64) g_patches.erase(g_patches.begin());
    g_patches.push_back(CameraPatch{});
    rec = &g_patches.back();
  }
  rec->buf = ub.buffer;
  rec->off = ub.offset;
  rec->frame = frame;
  memcpy(rec->orig, raw, 256);
  memcpy(rec->patched, m, 256);
  FEVERSCALER_LOG_N(2, "camera jitter applied: block %p+%llu, (%.3f, %.3f) px at %ux%u", (void*)ub.buffer,
              (unsigned long long)ub.offset, jx, jy, g_mainW, g_mainH);
}

// Streamline takes one set of common constants per frame (a second, different set is rejected).
// With DLSS the first set comes while the game's last command buffer is recorded; the pre-pass
// then keeps it.
static std::mutex g_constMx;
static uint64_t g_constFrame = ~0ull;
static bool SetFrameConstants(const sl::Constants& c) {
  std::lock_guard<std::mutex> lk(g_constMx);
  uint64_t f = g_frame.load();
  if (g_constFrame == f) return true;
  if (!SlSetConstants(c)) return false;
  g_constFrame = f;
  return true;
}

static bool CameraCut(const Camera& cur, const Camera& prev) {
  if (!cur.valid || !prev.valid) return true;
  float dx = cur.camPos[0] - prev.camPos[0], dy = cur.camPos[1] - prev.camPos[1], dz = cur.camPos[2] - prev.camPos[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz) > Cfg().cameraCutDistance;
}

// Streamline common constants (matrices without jitter; jitter separately, in render pixels).
static void FillConstants(sl::Constants& c, const Camera& cur, const Camera& prev, uint32_t w, uint32_t h, bool reset,
                          float jx, float jy) {
  DMat4 F = GlToVkClip();
  DMat4 currVP = DMat4::FromFloat(cur.projView), prevVP = DMat4::FromFloat(prev.projView);
  DMat4 c2p = Mul(Mul(F, prevVP), Inverse(Mul(F, currVP)));
  DMat4 viewToClip = Mul(F, DMat4::FromFloat(cur.proj));
  c.cameraViewToClip = ToSl(viewToClip);
  c.clipToCameraView = ToSl(Inverse(viewToClip));
  c.clipToLensClip = ToSl(DMat4::Identity());
  c.clipToPrevClip = ToSl(c2p);
  c.prevClipToClip = ToSl(Inverse(c2p));
  c.jitterOffset = sl::float2(jx, jy);
  c.mvecScale = sl::float2(1, 1);  // motion vectors are stored in UV units
  c.cameraPinholeOffset = sl::float2(0, 0);
  const float* vi = cur.viewInv;
  c.cameraPos = sl::float3(cur.camPos[0], cur.camPos[1], cur.camPos[2]);
  c.cameraRight = sl::float3(vi[0], vi[1], vi[2]);
  c.cameraUp = sl::float3(vi[4], vi[5], vi[6]);
  c.cameraFwd = sl::float3(-vi[8], -vi[9], -vi[10]);
  c.cameraNear = cur.nearZ;
  c.cameraFar = cur.farZ;
  c.cameraFOV = cur.fov;
  c.cameraAspectRatio = (float)w / (float)h;
  c.depthInverted = sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.orthographicProjection = sl::Boolean::eFalse;
  c.motionVectorsDilated = sl::Boolean::eFalse;
  c.motionVectorsJittered = sl::Boolean::eFalse;
}

// ---- instance / joint data --------------------------------------------------------------------------
static uint64_t MeshKey(const DrawRec& r) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix((uint64_t)r.pipeline);
  mix((uint64_t)r.posBuf);
  mix(r.posOff);
  mix((uint64_t)r.idxBuf);
  mix(r.idxOff);
  mix(r.first);
  mix(r.count);
  mix((uint64_t)(uint32_t)r.vertexOffset);
  return h;
}
static bool SameMat(const float* a, const float* b) { return memcmp(a, b, 64) == 0; }

static void QueueReplay(const DrawRec& rec, const float* cur, const float* prev, uint32_t n, bool instances, uint32_t order) {
  GpuSlot& s = GpuGetSlot(g_slot);
  if (!s.uploadPtr) return;
  if (s.uploadUsed + 2 * n > s.uploadCapacity) {
    LogEvent("motion-capacity", "motion replay skipped: matrices needed=%u available=%u", 2*n, s.uploadCapacity-s.uploadUsed);
    return;
  }
  float* dst = (float*)s.uploadPtr + (size_t)s.uploadUsed * 16;
  memcpy(dst, cur, (size_t)n * 64);
  float* pdst = dst + (size_t)n * 16;
  const float tele2 = Cfg().teleportDistance * Cfg().teleportDistance;
  uint32_t moving = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const float* c = &cur[(size_t)i * 16];
    const float* p = &prev[(size_t)i * 16];
    bool still = SameMat(c, p);
    if (!still && instances) {
      float dx = c[12] - p[12], dy = c[13] - p[13], dz = c[14] - p[14];
      still = dx * dx + dy * dy + dz * dz > tele2;
    }
    memcpy(pdst + (size_t)i * 16, still ? c : p, 64);
    if (!still) ++moving;
  }
  if (!moving) return;
  g_replays.push_back({rec, s.uploadUsed, n, order});
  s.uploadUsed += 2 * n;
}

// Stores n mat4 for `key` in this frame's arena; returns false if the read failed.
static bool StoreCurrent(const DrawRec& rec, uint32_t order, uint64_t key, uint32_t n, const std::function<bool(float*)>& fill) {
  MatStore& cs = g_store[g_cur];
  uint32_t off = (uint32_t)cs.arena.size();
  cs.arena.resize(cs.arena.size() + (size_t)n * 16);
  if (!fill(cs.arena.data() + off)) {
    cs.arena.resize(off);
    return false;
  }
  cs.rigid[key].push_back({rec, {off, n}, order});
  return true;
}
static void ProcessMesh(const DrawRec& rec, uint32_t order) {
  const PipeInfo& p = *rec.pipe;
  uint32_t n = rec.instanceCount;
  if (!rec.modelBuf || !n || n > 16384) return;
  VkDeviceSize base = rec.modelOff + (VkDeviceSize)rec.firstInstance * p.modelStride;
  bool packed = p.modelStride == 64 && p.modelOffs[0] == 0 && p.modelOffs[1] == 16 && p.modelOffs[2] == 32 &&
                p.modelOffs[3] == 48;
  uint64_t key = MeshKey(rec);
  StoreCurrent(rec, order, key, n, [&](float* dst) {
    if (packed) return ReadBufferBytes(rec.modelBuf, base, dst, (size_t)n * 64);
    static thread_local std::vector<uint8_t> tmp;
    size_t bytes = (size_t)n * p.modelStride;
    tmp.resize(bytes);
    if (!ReadBufferBytes(rec.modelBuf, base, tmp.data(), bytes)) return false;
    for (uint32_t i = 0; i < n; ++i)
      for (int c = 0; c < 4; ++c) memcpy(dst + (size_t)i * 16 + c * 4, tmp.data() + (size_t)i * p.modelStride + p.modelOffs[c], 16);
    return true;
  });

}

constexpr float kSkinnedMatchDistance = 5.0f;  // metres a skinned mesh's root joint may move per frame

static void ProcessSkinned(const DrawRec& rec, uint32_t order) {
  UboBinding ub;
  if (!rec.set3 || !GetUboBinding(rec.set3, 11, &ub)) return;
  VkDeviceSize off = ub.offset + (ub.dynamic ? rec.dyn3 : 0);
  VkDeviceSize range = (ub.range == VK_WHOLE_SIZE || ub.range > 45 * 64) ? 45 * 64 : ub.range;
  uint32_t jn = (uint32_t)(range / 64);
  if (!jn) return;
  MatStore& cs = g_store[g_cur];
  uint32_t at = (uint32_t)cs.arena.size();
  cs.arena.resize(cs.arena.size() + (size_t)jn * 16);
  if (!ReadBufferBytes(ub.buffer, off, cs.arena.data() + at, (size_t)jn * 64)) {
    cs.arena.resize(at);
    return;
  }
  uint64_t key = MeshKey(rec);
  cs.skinned[key].push_back(MatRange{at, jn});
  // Last frame's joint block of the same mesh whose root joint is nearest to this one.
  const MatStore& ps = g_store[g_cur ^ 1];
  auto it = ps.skinned.find(key);
  if (it == ps.skinned.end()) return;
  const float* cur = cs.arena.data() + at;
  const float* best = nullptr;
  float bestD2 = kSkinnedMatchDistance * kSkinnedMatchDistance;
  for (const MatRange& r : it->second) {
    if (r.n != jn) continue;
    const float* p = ps.arena.data() + r.off;
    float dx = cur[12] - p[12], dy = cur[13] - p[13], dz = cur[14] - p[14];
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 <= bestD2) {
      bestD2 = d2;
      best = p;
    }
  }
  if (!best || !memcmp(best, cur, (size_t)jn * 64)) return;
  QueueReplay(rec, cur, best, jn, false, order);
  ++g_replaySkinned;
}

// Resolve history after all main-pass draws have been captured. Pooling by geometry
// makes matching independent of draw order, batch boundaries, and instance counts.
static void BuildRigidReplays() {
  if (g_rigidReplaysBuilt) return;
  g_rigidReplaysBuilt = true;
  MatStore& cs = g_store[g_cur];
  const MatStore& ps = g_store[g_cur ^ 1];
  static std::vector<const float*> current, previous;
  static std::vector<int32_t> matches;
  static std::vector<float> alignedPrevious;
  for (const auto& group : cs.rigid) {
    auto old = ps.rigid.find(group.first);
    if (old == ps.rigid.end() || old->second.empty()) continue;
    current.clear(); previous.clear();
    for (const auto& draw : group.second) for (uint32_t i = 0; i < draw.matrices.n; ++i)
      current.push_back(cs.arena.data() + draw.matrices.off + (size_t)i * 16);
    for (const auto& draw : old->second) for (uint32_t i = 0; i < draw.matrices.n; ++i)
      previous.push_back(ps.arena.data() + draw.matrices.off + (size_t)i * 16);
    MatchMotionInstances(current, previous, std::min(5.0f, Cfg().teleportDistance), matches);
    size_t base = 0;
    for (const auto& draw : group.second) {
      const uint32_t n = draw.matrices.n;
      bool moving = false;
      for (uint32_t i = 0; i < n; ++i) {
        int32_t oldIndex = matches[base + i];
        if (oldIndex >= 0 && !SameMat(current[base + i], previous[oldIndex])) { moving = true; break; }
      }
      if (moving) {
        alignedPrevious.resize((size_t)n * 16);
        for (uint32_t i = 0; i < n; ++i) {
          int32_t oldIndex = matches[base + i];
          // New/ambiguous objects retain camera motion instead of borrowing a neighbor's history.
          const float* matrix = oldIndex >= 0 ? previous[oldIndex] : current[base + i];
          memcpy(alignedPrevious.data() + (size_t)i * 16, matrix, 64);
        }
        size_t before = g_replays.size();
        QueueReplay(draw.rec, cs.arena.data() + draw.matrices.off, alignedPrevious.data(), n, true, draw.order);
        if (before != g_replays.size()) ++g_replayMesh;
      }
      base += n;
    }
  }
  // Deferred rigid matching must not change which overlapping surface writes motion last.
  std::sort(g_replays.begin(), g_replays.end(), [](const Replay& a, const Replay& b) { return a.order < b.order; });
}

static void BeginFrameLocked() {
  g_frameOpen = true;
  g_mainPassesOpen = 0;
  g_slot = (uint32_t)(g_frame.load() % kSlots);
  GpuSlot& s = GpuGetSlot(g_slot);
  GpuWaitSlot(s);
  s.uploadUsed = 0;
  g_replays.clear();
  g_replayMesh = g_replaySkinned = 0;
  g_motionDrawOrder = 0;
  g_rigidReplaysBuilt = false;
  g_store[g_cur].Clear();
  g_cam.valid = false;
  g_haveMain = false;
  g_tagged = false;
  GpuCollectGarbage(g_frame.load());
}

static void ProcessCommandBuffer(CbState* st) {
  if (st->hasMainPass) {
    g_haveMain = true;
    g_mainDepth = st->mainDepth;
    g_mainDepthFmt = st->mainDepthFormat;
    g_mainSamples = st->mainSamples;
    g_mainW = st->mainW;
    g_mainH = st->mainH;
  }
  if (!g_haveMain || g_mainSamples != VK_SAMPLE_COUNT_1_BIT) return;
  if (st->viewSet) CameraBlockSeen(st->viewSet);
  for (const DrawRec& rec : st->draws) {
    if (!g_cam.valid && rec.pipe->usesView && rec.set0) CameraBlockSeen(rec.set0);
    if (!Cfg().objectMotion || !GpuReady()) continue;
    uint32_t order = g_motionDrawOrder++;
    if (rec.pipe->kind == PipeInfo::Mesh) ProcessMesh(rec, order);
    else if (rec.pipe->kind == PipeInfo::Skinned && Cfg().skinnedMotion) ProcessSkinned(rec, order);
  }
  // Every other copy of the main camera (reflections, fog, water...) gets the same jitter, so the
  // whole image DLSS sees moves together.
  for (uint32_t i = 0; i < st->otherViewCount; ++i) CameraBlockSeen(st->otherViewSets[i], false);
}

// ---- pre-pass: depth copy + motion vectors ----------------------------------------------------------
static void Barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags ss, VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = sa;
  b.dstAccessMask = da;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {aspect, 0, 1, 0, 1};
  vk.vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool DepthHasStencil(VkFormat f) {
  return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

static void RunPrePass(VkQueue queue, CbState* uiSt) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!RenderWorkRequested() || g_tagged || !g_haveMain || !g_cam.valid || !g_mainDepth || !GpuReady()) return;
  ImageDesc source;
  if (!GetImageDesc(g_mainDepth, &source) || source.samples != VK_SAMPLE_COUNT_1_BIT ||
      !(source.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return;
  GpuSlot& s = GpuGetSlot(g_slot);
  if (!GpuEnsureRenderTargets(s, g_mainW, g_mainH, g_mainDepthFmt)) return;
  GpuUpdateDescriptors(s);
  BuildRigidReplays();

  // Camera matrices. Previous = last tagged frame's camera, unless that is missing or a cut.
  bool reset = !g_prevCam.valid || g_prevW != g_mainW || g_prevH != g_mainH;
  if (!reset) {
    float dx = g_cam.camPos[0] - g_prevCam.camPos[0], dy = g_cam.camPos[1] - g_prevCam.camPos[1],
          dz = g_cam.camPos[2] - g_prevCam.camPos[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > Cfg().cameraCutDistance) reset = true;
  }
  const Camera& prev = reset ? g_cam : g_prevCam;
  // The game rendered this frame with the jittered camera: reconstruct positions from the depth
  // with that, report motion without it.
  float jx, jy;
  bool jittered = FrameJitter(&jx, &jy);
  float currJit[16];
  memcpy(currJit, g_cam.projView, 64);
  if (jittered) JitterMat(currJit, 2.0f * jx / (float)g_mainW, -2.0f * jy / (float)g_mainH);
  DMat4 F = GlToVkClip();
  DMat4 prevVP = DMat4::FromFloat(prev.projView);
  DMat4 c2p = Mul(Mul(F, prevVP), Inverse(Mul(F, DMat4::FromFloat(currJit))));
  FrameUbo u{};
  memcpy(u.currVP, g_cam.projView, 64);
  memcpy(u.prevVP, prev.projView, 64);
  ToFloat(c2p, u.clipToPrevClip);
  memcpy(u.currVPJit, currJit, 64);
  u.jitter[0] = jittered ? 2.0f * jx / (float)g_mainW : 0.0f;  // raster NDC, y down
  u.jitter[1] = jittered ? 2.0f * jy / (float)g_mainH : 0.0f;
  u.size[0] = (float)g_mainW;
  u.size[1] = (float)g_mainH;
  u.size[2] = 1.0f / g_mainW;
  u.size[3] = 1.0f / g_mainH;
  u.nearFar[0] = g_cam.nearZ;
  u.nearFar[1] = g_cam.farZ;
  memcpy(s.uboPtr, &u, sizeof(u));

  VkCommandBuffer cmd = s.cmd;
  vk.vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk.vkBeginCommandBuffer(cmd, &bi);
  VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | (DepthHasStencil(g_mainDepthFmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
  // Between its render passes the game keeps the scene depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
  Barrier(cmd, g_mainDepth, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT);
  Barrier(cmd, s.depth.image, dsAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkImageCopy cp{};
  cp.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  cp.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  cp.extent = {g_mainW, g_mainH, 1};
  vk.vkCmdCopyImage(cmd, g_mainDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.depth.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
  Barrier(cmd, g_mainDepth, dsAspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  Barrier(cmd, s.depth.image, dsAspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  Barrier(cmd, s.mv.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);

  // Camera motion for every pixel.
  vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, GpuCameraPipeline());
  vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, GpuPipelineLayout(), 0, 1, &s.set, 0, nullptr);
  vk.vkCmdDispatch(cmd, (g_mainW + 7) / 8, (g_mainH + 7) / 8, 1);

  // Object motion: replay the draws whose transforms changed since last frame.
  uint32_t drawn = 0;
  if (!reset && !g_replays.empty()) {
    Barrier(cmd, s.mv.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = GpuMvRenderPass();
    rb.framebuffer = s.mvFb;
    rb.renderArea = {{0, 0}, {g_mainW, g_mainH}};
    vk.vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)g_mainW, (float)g_mainH, 0, 1};
    VkRect2D sc{{0, 0}, {g_mainW, g_mainH}};
    vk.vkCmdSetViewport(cmd, 0, 1, &vp);
    vk.vkCmdSetScissor(cmd, 0, 1, &sc);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, GpuPipelineLayout(), 0, 1, &s.set, 0, nullptr);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const Replay& r : g_replays) {
      const DrawRec& d = r.rec;
      VkPipeline pipe = GpuReplayPipeline(*d.pipe);
      if (!pipe) continue;
      if (pipe != bound) {
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        bound = pipe;
      }
      bool skinned = d.pipe->kind == PipeInfo::Skinned;
      VkBuffer vbs[2] = {d.posBuf, d.inflBuf};
      VkDeviceSize offs[2] = {d.posOff, d.inflOff};
      if (skinned && !d.inflBuf) continue;
      vk.vkCmdBindVertexBuffers(cmd, 0, skinned ? 2 : 1, vbs, offs);
      uint32_t pc[2] = {r.base, r.count};
      vk.vkCmdPushConstants(cmd, GpuPipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
      uint32_t instances = skinned ? d.instanceCount : r.count;
      if (d.indexed) {
        if (!d.idxBuf) continue;
        vk.vkCmdBindIndexBuffer(cmd, d.idxBuf, d.idxOff, d.idxType);
        vk.vkCmdDrawIndexed(cmd, d.count, instances, d.first, d.vertexOffset, 0);
      } else {
        vk.vkCmdDraw(cmd, d.count, instances, d.first, 0);
      }
      ++drawn;
    }
    vk.vkCmdEndRenderPass(cmd);  // -> SHADER_READ_ONLY_OPTIMAL
  } else {
    Barrier(cmd, s.mv.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_SHADER_READ_BIT);
  }
  vk.vkEndCommandBuffer(cmd);
  vk.vkResetFences(gDevice, 1, &s.fence);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VkResult sr;
  { DiagnosticScope scope(DiagnosticStage::Prepass); sr = vk.vkQueueSubmit(queue, 1, &si, s.fence); }
  if (sr != VK_SUCCESS) {
    LogEvent("prepass-submit", "pre-pass vkQueueSubmit failed result=%d", (int)sr);
    return;
  }
  s.pending = true;
  s.depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  s.mv.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Inputs for DLSS-G.
  SlImage depth{s.depth.image, s.depth.view, s.depth.mem, s.depth.format, s.depth.layout, s.depth.usage, g_mainW, g_mainH};
  SlImage mv{s.mv.image, s.mv.view, s.mv.mem, s.mv.format, s.mv.layout, s.mv.usage, g_mainW, g_mainH};
  SlImage hud{};
  bool haveHud = false;
  if (Cfg().tagHudless && uiSt && uiSt->hudlessSlot >= 0) {
    GpuSlot& hs = GpuGetSlot((uint32_t)uiSt->hudlessSlot);
    if (hs.hudless.image) {
      hud = SlImage{hs.hudless.image, hs.hudless.view, hs.hudless.mem, hs.hudless.format,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, hs.hudless.usage, hs.hudless.w, hs.hudless.h};
      haveHud = true;
    }
  }
  bool tagged = SlTagFrame(depth, mv, haveHud ? &hud : nullptr);

  sl::Constants c{};
  FillConstants(c, g_cam, prev, g_mainW, g_mainH, reset, jittered ? jx : 0.0f, jittered ? jy : 0.0f);
  bool consts = SetFrameConstants(c);
  g_tagged = tagged && consts;

  static uint32_t statFrames = 0, statMesh = 0, statSkinned = 0, statDrawn = 0;
  ++statFrames;
  statMesh += g_replayMesh;
  statSkinned += g_replaySkinned;
  statDrawn += drawn;
  if (statFrames == 1 || statFrames % 600 == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double procMs = g_cpuProcessTicks.exchange(0) * 1000.0 / f.QuadPart / statFrames;
    double preMs = g_cpuPrePassTicks.exchange(0) * 1000.0 / f.QuadPart / statFrames;
    Log("frame %llu: render %ux%u hudless=%d reset=%d | per-frame avg over %u: moving mesh draws %.1f, skinned %.1f, "
        "replayed %.1f, CPU %.2f ms (matrices %.2f, pre-pass %.2f)",
        (unsigned long long)g_frame.load(), g_mainW, g_mainH, haveHud, reset, statFrames, statMesh / (double)statFrames,
        statSkinned / (double)statFrames, statDrawn / (double)statFrames, procMs + preMs, procMs, preMs);
    if (statFrames > 1) statFrames = statMesh = statSkinned = statDrawn = 0;
  }
}

// ---- UI pass: DLSS + HUD-less copy (recorded into the game's command buffer) ------------------------
// Records DLSS into the game's command buffer: the post-compose scene copy (render size) -> our
// display-size image. Depth and motion vectors come from this frame's pre-pass, which is submitted
// before this command buffer.
static bool RecordDlss(VkCommandBuffer cb, CbState* st, GpuSlot& s, GpuImage& out, const Camera& cur,
                       const Camera& prev, bool camThisFrame) {
  if (!SlDlssAvailable() || !st->srIn || !cur.valid) return false;
  if (st->srInLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && st->srInLayout != VK_IMAGE_LAYOUT_GENERAL) {
    FEVERSCALER_LOG_N(3, "DLSS skipped: scene copy in layout %d", (int)st->srInLayout);
    return false;
  }
  if (!s.depth.image || !s.mv.image || s.depth.w != st->srInW || s.depth.h != st->srInH) return false;
  ImageDesc in;
  if (!GetImageDesc(st->srIn, &in) || !in.view || in.samples != VK_SAMPLE_COUNT_1_BIT) return false;
  if (!SlDlssConfigure(st->srInW, st->srInH, out.w, out.h, Cfg().dlssPreset)) return false;
  if (st->srInW != g_srRenderW || st->srInH != g_srRenderH) g_srResetNext = true;  // new render size
  float jx, jy;
  bool jittered = FrameJitter(&jx, &jy);
  bool reset = g_srResetNext.load() || CameraCut(cur, prev);
  sl::Constants c{};
  FillConstants(c, cur, prev, st->srInW, st->srInH, reset, jittered ? jx : 0.0f, jittered ? jy : 0.0f);
  if (!SetFrameConstants(c)) return false;
  FEVERSCALER_LOG_N(3, "DLSS constants set while recording, camera from %s", camThisFrame ? "this frame" : "the previous frame");
  Barrier(cb, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  SlImage inI{st->srIn, in.view, VK_NULL_HANDLE, in.format, st->srInLayout, in.usage, st->srInW, st->srInH};
  SlImage outI{out.image, out.view, out.mem, out.format, VK_IMAGE_LAYOUT_GENERAL, out.usage, out.w, out.h};
  SlImage dI{s.depth.image, s.depth.view, s.depth.mem, s.depth.format, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
             s.depth.usage, s.depth.w, s.depth.h};
  SlImage mI{s.mv.image, s.mv.view, s.mv.mem, s.mv.format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s.mv.usage,
             s.mv.w, s.mv.h};
  if (!SlDlssEvaluate(cb, inI, outI, dI, mI)) return false;
  g_srResetNext = false;
  g_srRenderW = st->srInW;
  g_srRenderH = st->srInH;
  g_srOutW = out.w;
  g_srOutH = out.h;
  return true;
}

// Device capability is separate from the user's switches: present still services the menu.
static bool FrameActive() { return SlCoreReady() && (SlFgReady() || SlDlssAvailable()); }

void OnUiPassBegin(VkCommandBuffer cb, CbState* st) {
  if (!GpuReady() || !FrameActive()) return;
  if (!RenderWorkRequested() && g_redirLinger <= 0) return;
  ImageDesc color;
  if (!GetImageDesc(st->candImage, &color) || color.samples != VK_SAMPLE_COUNT_1_BIT ||
      !(color.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return;
  VkExtent2D se = SwapchainExtent();
  VkFormat sf = SwapchainFormat();
  if (st->candLayout == VK_IMAGE_LAYOUT_UNDEFINED || st->candLayout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
      !st->candW || !st->candH || sf == VK_FORMAT_UNDEFINED) {
    FEVERSCALER_LOG_N(5, "UI pass: no usable scene image %p %ux%u layout %d, swapchain %ux%u fmt %d", (void*)st->candImage,
                st->candW, st->candH, (int)st->candLayout, se.width, se.height, (int)sf);
    return;
  }
  uint32_t slot = RecordingSlot();
  GpuSlot& s = GpuGetSlot(slot);
  bool sr = g_srOn.load() && SrRequested();
  bool hud = Cfg().tagHudless && Cfg().frameGeneration && SlFgReady();
  GpuImage* out = nullptr;
  Camera cur, prev;
  bool camThisFrame;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    ImageDesc depth;
    // UI recording can precede this frame's 3D submit. Use the recorded world depth for the
    // sample-count guard; submit-time state alone would spuriously disable SR every few frames.
    VkImage depthImage = st->hasMainPass ? st->mainDepth : WorldDepthImage();
    if (!GetImageDesc(depthImage, &depth) || depth.samples != VK_SAMPLE_COUNT_1_BIT ||
        !(depth.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      sr = hud = false;
      SetSceneRedirect(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    if (hud && !GpuEnsureHudless(s, se.width, se.height, sf)) hud = false;
    if (sr || g_redirLinger > 0) out = GpuEnsureSrOutput(se.width, se.height);
    // This frame's camera if its 3D submits came first, else last frame's (DLSS mostly needs the
    // jitter and the motion vectors; the pre-pass sets the exact constants for DLSS-G later).
    camThisFrame = g_cam.valid && g_camFrame == g_frame.load();
    cur = g_cam.valid ? g_cam : g_prevCam;
    prev = (g_cam.valid && g_prevCam.valid) ? g_prevCam : cur;
  }
  // The copies run in the game's command buffer: transfer commands and barriers only (DLSS binds
  // compute state, which the game does not use).
  auto blit = [&](VkImage src, uint32_t sw, uint32_t sh, VkImage dst, uint32_t dw, uint32_t dh) {
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(int32_t)sw, (int32_t)sh, 1};
    b.dstOffsets[1] = {(int32_t)dw, (int32_t)dh, 1};
    vk.vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                      (sw == dw && sh == dh) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  };
  VkImage hudSrc = st->candImage;
  uint32_t hudW = st->candW, hudH = st->candH;
  VkImageLayout hudSrcLayout = st->candLayout;
  bool dlssOk = false;
  if (out) {
    if (sr) dlssOk = RecordDlss(cb, st, s, *out, cur, prev, camThisFrame);
    VkImageLayout outLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!dlssOk) {
      // No DLSS image this frame, but the UI pass may already sample ours: fill it with the game's
      // own picture, stretched the way the UI pass would have.
      Barrier(cb, out->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      Barrier(cb, st->candImage, VK_IMAGE_ASPECT_COLOR_BIT, st->candLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
      blit(st->candImage, st->candW, st->candH, out->image, out->w, out->h);
      Barrier(cb, st->candImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st->candLayout,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
      outLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    Barrier(cb, out->image, VK_IMAGE_ASPECT_COLOR_BIT, outLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    hudSrc = out->image;
    hudW = out->w;
    hudH = out->h;
    hudSrcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!sr && g_redirLinger > 0) --g_redirLinger;
    if (dlssOk) g_dlssFrameOk = true;
    FEVERSCALER_LOG_N(1, "UI pass: %s -> DLSS output %ux%u", dlssOk ? "DLSS evaluated" : "plain upscale", out->w, out->h);
  }
  if (hud) {
    if (hudSrc == st->candImage)
      Barrier(cb, st->candImage, VK_IMAGE_ASPECT_COLOR_BIT, st->candLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
    Barrier(cb, s.hudless.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    blit(hudSrc, hudW, hudH, s.hudless.image, se.width, se.height);
    if (hudSrc == st->candImage)
      Barrier(cb, st->candImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st->candLayout,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
    Barrier(cb, s.hudless.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_READ_BIT);
    st->hudlessSlot = (int)slot;
    FEVERSCALER_LOG_N(1, "HUD-less copy recorded (source %ux%u -> slot %u)", hudW, hudH, slot);
  }
  if (out) {
    // Hand-off to the UI pass. If the image it stretches onto the screen is already display-sized
    // (DLAA, or the game's FSR1 upscaling to full size) and writable, copy our result into it: at
    // 100 % scale that image is also the scene copy the tonemapper reads, so redirecting its
    // descriptors would feed our output back into the game's post chain. Otherwise (render-size
    // image) the UI pass samples our image through redirected descriptor writes.
    ImageDesc cd;
    bool copyBack = sr && GetImageDesc(st->candImage, &cd) && cd.w == out->w && cd.h == out->h &&
                    (cd.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (copyBack) {
      Barrier(cb, st->candImage, VK_IMAGE_ASPECT_COLOR_BIT, st->candLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      blit(out->image, out->w, out->h, st->candImage, cd.w, cd.h);
      Barrier(cb, st->candImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, st->candLayout,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_ACCESS_SHADER_READ_BIT);
      SetSceneRedirect(VK_NULL_HANDLE, VK_NULL_HANDLE);
      FEVERSCALER_LOG_N(1, "DLSS output handed over by copying into the game's %ux%u scene image", cd.w, cd.h);
    } else if (sr) {
      SetSceneRedirect(st->candImage, out->view);
    }
    Barrier(cb, out->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  }
  (void)hudSrcLayout;
}

// ---- DLSS Super Resolution control (present thread) ---------------------------------------------------
bool SrActive() { return g_srOn.load() && g_dlssLive.load() && SrRequested(); }

SrStatus SrGetStatus() {
  SrStatus s;
  s.available = SlDlssAvailable() && !g_srFailed.load();
  s.unavailableReason = g_srFailed.load() ? "DLSS stopped producing frames; restart the game to retry" : SlSrUnavailableReason();
  s.wanted = g_srWanted;
  s.active = g_srOn.load() && g_dlssLive.load();
  s.mode = SlDlssModeName();
  s.renderW = g_srRenderW;
  s.renderH = g_srRenderH;
  s.outW = g_srOutW;
  s.outH = g_srOutH;
  s.preset = Cfg().dlssPreset;
  s.renderMode = Cfg().dlssMode;
  s.scaleControl = GameScaleAvailable();
  s.sliderScale = GameSliderScale();
  return s;
}

void SrSetMode(uint32_t mode) {
  if (mode >= kDlssModeCount || mode == Cfg().dlssMode) return;
  Log("user: render resolution %s", DlssModeName(mode));
  SaveDlssMode(mode);  // SrOnPresent hands the scale to the game
  g_srResetNext = true;
}

void SrSetPreset(uint32_t preset) {
  if (preset == Cfg().dlssPreset) return;
  Log("user: DLSS preset %u", preset);
  SaveDlssPreset(preset);  // SlDlssConfigure picks it up at the next evaluate
  g_srResetNext = true;
}

void SrSetEnabled(bool on) {
  if (on == g_srWanted) return;
  g_srWanted = on;
  Log("user: DLSS Super Resolution %s", on ? "on" : "OFF");
  SaveSuperResolution(on);
}

// Applies the SR switch between frames and tracks whether DLSS is producing the image.
// frame3D: a world pass was seen, including one whose depth cannot be used by DLSS.
static void SrOnPresent(bool frame3D) {
  bool want = g_srWanted && SlDlssAvailable();
  bool dlssFrame = g_dlssFrameOk.exchange(false);
  switch (g_srScale.Update(want, dlssFrame, frame3D)) {
    case SrScalePolicy::Event::Proven: Log("DLSS produces frames: the DLSS preset may set the render resolution"); break;
    case SrScalePolicy::Event::GaveUp:
      g_srFailed = true;
      Log("DLSS made no image for %u frames of the 3D view: SR disabled and game settings restored for this session",
          SrScalePolicy::kFailFrames);
      break;
    default: break;
  }
  want = want && !g_srFailed.load();
  // Render resolution: the DLSS preset's scale while DLSS upscales, the game's slider otherwise.
  GameSetScaleOverride(g_srScale.PresetScaleAllowed(want) && Cfg().dlssMode ? DlssModeScale(Cfg().dlssMode) : 0.0f);
  GameScalePoll();
  if (want != g_srOn.load()) {
    g_srOn = want;
    g_srResetNext = true;
    g_dlssMisses = 0;
    if (!want) {
      g_dlssLive = false;
      SetSceneRedirect(VK_NULL_HANDLE, VK_NULL_HANDLE);
      g_redirLinger = 3;  // descriptor writes already redirected for frames in flight
    }
    Log("DLSS Super Resolution %s", want ? "on" : "off");
  }
  if (g_srOn.load()) {
    if (dlssFrame) {
      g_dlssMisses = 0;
      if (!g_dlssLive.load()) {
        g_dlssLive = true;
        g_srResetNext = true;
        Log("DLSS producing frames: camera jitter on");
      }
    } else if (++g_dlssMisses >= 3 && g_dlssLive.load()) {
      g_dlssLive = false;
      Log("DLSS idle for 3 frames (menu/loading or error): camera jitter off");
    }
  }
  MipBiasOnPresent(SrActive() && g_srOutW ? (float)g_srRenderW / (float)g_srOutW : 0.0f, g_frame.load());
  std::lock_guard<std::mutex> lk(g_mx);
  if (g_mainW && g_mainH) {
    g_jitterW = g_mainW;
    g_jitterH = g_mainH;
  }
}

// ---- wrappers ------------------------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL w_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* submits, VkFence fence) {
  if (!RenderWorkRequested() || !gDevice) return vk.vkQueueSubmit(q, n, submits, fence);
  DiagnosticScope scope(DiagnosticStage::Submit);
  if (!GpuReady()) GpuInit(gQueueFamily);
  CbState* uiSt = nullptr;
  LARGE_INTEGER t0, t1, t2;
  QueryPerformanceCounter(&t0);
  {
    std::lock_guard<std::mutex> lk(g_mx);
    bool first = !g_frameOpen;
    if (first) {
      BeginFrameLocked();
    } else if (g_mainPassesOpen > kMaxMainPassesPerFrame || g_store[g_cur].arena.size() > kMaxFrameMatrixFloats) {
      // 3D frames without presents: start the frame over rather than collect their matrices without bound.
      static bool logged = false;
      if (!logged) {
        logged = true;
        Log("frame: %u main passes without a present (minimised?) - frame data started over (logged once)",
            g_mainPassesOpen);
      }
      BeginFrameLocked();
    }
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t c = 0; c < submits[i].commandBufferCount; ++c) {
        CbState* st = GetCbState(submits[i].pCommandBuffers[c]);
        ProcessCommandBuffer(st);
        if (st->hasMainPass) ++g_mainPassesOpen;
        if (st->uiPassSeen) uiSt = st;
      }
    if (first) SlOnFirstSubmit();
  }
  QueryPerformanceCounter(&t1);
  if (uiSt) RunPrePass(q, uiSt);
  QueryPerformanceCounter(&t2);
  g_cpuProcessTicks += t1.QuadPart - t0.QuadPart;
  g_cpuPrePassTicks += t2.QuadPart - t1.QuadPart;
  VkResult result = vk.vkQueueSubmit(q, n, submits, fence);
  if (result != VK_SUCCESS) LogEvent("game-submit", "vkQueueSubmit result=%d", (int)result);
  return result;
}

// TF3 keeps rendering in the background, where the key belongs to other programs.
static bool GameInForeground() {
  DWORD pid = 0;
  if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

// The dev menu key. Followed in the background too, so a key still held when the game gets the
// focus back does not fire.
static void PollMenuKey() {
  static bool was = false;
  int key = Cfg().keyMenu;
  auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
  bool down = (key & 0xFFFF) && held(key & 0xFFFF) && (!(key & kKeyCtrl) || held(VK_CONTROL)) &&
              (!(key & kKeyShift) || held(VK_SHIFT)) && (!(key & kKeyAlt) || held(VK_MENU));
  if (down && !was && GameInForeground()) OverlayToggle();
  was = down;
}

static uint32_t MaxGeneratedFrames() {
  uint32_t mx = SlFramesMax();
  if (!mx) return 1;
  return mx < kMaxGeneratedFrames ? mx : kMaxGeneratedFrames;
}

FgStatus FgGetStatus() {
  FgStatus s;
  s.available = SlFgReady();
  s.unavailableReason = SlFgUnavailableReason();
  s.userEnabled = !g_userOff;
  s.active = g_genOn;
  s.have3D = g_lastValid;
  s.generatedFrames = std::min(g_genFrames, MaxGeneratedFrames());
  s.maxGeneratedFrames = MaxGeneratedFrames();
  s.baseFps = g_baseFps;
  return s;
}

void FgSetUserEnabled(bool on) {
  if (on == !g_userOff) return;
  g_userOff = !on;
  Log("user: DLSS-G %s", on ? "on" : "OFF");
  SaveFrameGeneration(on);
  if (g_userOff) {
    g_genOn = false;
    SlSetGeneration(false, g_genFrames, true);  // user off: give its VRAM back
  }
  g_validStreak = 0;  // back on: the gate re-enables it after OnAfterFrames valid frames
}

void FgSetGeneratedFrames(uint32_t frames) {
  uint32_t mx = MaxGeneratedFrames();
  if (frames < 1) frames = 1;
  if (frames > mx) frames = mx;
  if (frames == g_genFrames) return;
  g_genFrames = frames;
  Log("user: generated frames -> %u (x%u, max x%u)", frames, frames + 1, mx + 1);
  if (g_genOn) {
    g_genOn = SlSetGeneration(true, g_genFrames);
    if (!g_genOn) g_validStreak = 0;
  }
  SaveGeneratedFrames(frames);
}

static void Gate(bool valid) {
  if (g_userOff || !SlFgReady()) return;
  if (valid) {
    g_idleStreak = 0;
    if (g_validStreak < 1000000) ++g_validStreak;
    if (!g_genOn && g_validStreak >= Cfg().onAfterFrames) {
      Log("gate: %u frames with valid inputs -> DLSS-G on", g_validStreak);
      g_genOn = SlSetGeneration(true, g_genFrames);
      if (!g_genOn) g_validStreak = 0;  // retry after another valid streak, not on every present
    }
  } else {
    g_validStreak = 0;
    if (g_idleStreak < 1000000) ++g_idleStreak;
    if (g_genOn && g_idleStreak >= Cfg().offAfterIdleFrames) {
      g_genOn = false;
      Log("gate: %u presents without a 3D frame -> DLSS-G suspended", g_idleStreak);
      SlSetGeneration(false, g_genFrames);
    }
  }
}

// Every 600 presents: the game's frame times and VRAM use, for stutter and performance reports.
static void LogFrameTimes(float ms) {
  static float times[600];
  static unsigned count = 0;
  times[count++] = ms;
  if (count < std::size(times)) return;
  count = 0;
  double total = 0;
  for (float t : times) total += t;
  float* p99 = times + std::size(times) * 99 / 100;
  std::nth_element(times, p99, std::end(times));
  char vram[64] = "";
  VramInfo v = QueryVram();
  if (v.valid) snprintf(vram, sizeof(vram), "; VRAM %.2f of %.2f GiB", v.usage / 1073741824.0, v.budget / 1073741824.0);
  Log("frame times over %zu presents (%.1f s): average %.1f ms, 99th percentile %.1f ms, longest %.1f ms%s",
      std::size(times), total / 1000, total / std::size(times), *p99, *std::max_element(p99, std::end(times)), vram);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
  DiagnosticScope scope(DiagnosticStage::Present);
  static bool once = false;
  if (!once) {
    once = true;
    g_genFrames = Cfg().generatedFrames;
    g_userOff = !Cfg().frameGeneration;
    g_srWanted = Cfg().superResolution;
    Log("first present (queue %p, %u wait semaphores)", (void*)q, pi ? pi->waitSemaphoreCount : 0);
    if (!FrameActive())
      Log("DLSS unavailable, the game's frames stay untouched (Super Resolution: %s; frame generation: %s)",
          SlSrUnavailableReason(), SlFgUnavailableReason());
  }
  if (!FrameActive()) {
    // The game's own present. The dev menu and the settings page still work and say why.
    PollMenuKey();
    BridgePoll();
    VkPresentInfoKHR present = *pi;
    OverlayPresent(q, &present);
    return vk.vkQueuePresentKHR(q, &present);
  }
  if (SlFgReady()) {
    uint32_t oldMax = SlFramesMax();
    SlPollState();  // capabilities must be known before the UI, bridge and gate use them
    if (g_genOn && oldMax != SlFramesMax()) {
      g_genOn = SlSetGeneration(true, g_genFrames);
      if (!g_genOn) g_validStreak = 0;
    }
  }
  PollMenuKey();
  BridgePoll();
  GameSettingsPoll();
  {
    static LARGE_INTEGER last{}, freq{};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    if (last.QuadPart) {
      double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
      if (dt > 0) {
        g_baseFps = g_baseFps > 0 ? g_baseFps + 0.05f * ((float)(1.0 / dt) - g_baseFps) : (float)(1.0 / dt);
        LogFrameTimes((float)(dt * 1000));
      }
    }
    last = now;
  }
  bool valid, worldSeen, unsafeDepth;
  DiagnosticContext diagnostic;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    valid = RenderWorkRequested() && g_tagged;
    worldSeen = RenderWorkRequested() && g_haveMain;
    unsafeDepth = worldSeen && g_mainSamples != VK_SAMPLE_COUNT_1_BIT;
    diagnostic.renderW = g_mainW; diagnostic.renderH = g_mainH;
  }
  g_lastValid = valid;
  if (!valid) SlClearTags();
  if (unsafeDepth && g_genOn) {
    g_genOn = false;
    SlSetGeneration(false, g_genFrames);
  }
  Gate(valid);
  diagnostic.fgOn = g_genOn;
  diagnostic.multiplier = (g_genFrames < MaxGeneratedFrames() ? g_genFrames : MaxGeneratedFrames()) + 1;
  diagnostic.srOn = g_srOn.load(); diagnostic.srMode = Cfg().dlssMode; diagnostic.srPreset = Cfg().dlssPreset;
  diagnostic.sceneValid = valid;
  auto extent = SwapchainExtent(); diagnostic.outputW = extent.width; diagnostic.outputH = extent.height;
  DiagnosticsContext(diagnostic);
  // The dev menu is drawn into the image being presented (may swap the wait semaphores).
  VkPresentInfoKHR present = *pi;
  OverlayPresent(q, &present);
  SlOnPresentBegin();
  VkResult r = vk.vkQueuePresentKHR(q, &present);
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) LogEvent("present-result", "vkQueuePresentKHR result=%d", (int)r);
  SlOnPresentEnd();
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (valid) {
      g_prevCam = g_cam;
      g_prevW = g_mainW;
      g_prevH = g_mainH;
      g_cur ^= 1;  // this frame's matrices become "previous"; the other store is cleared at the next frame start
    } else {
      g_prevCam.valid = false;
      g_store[g_cur ^ 1].Clear();
    }
    g_frameOpen = false;
    g_frame.fetch_add(1);
  }
  SrOnPresent(worldSeen);
  GameSettingsPoll();  // an SR failure must release its MSAA override as well
  FEVERSCALER_LOG_N(3, "present -> %d", (int)r);
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci,
                                                           const VkAllocationCallbacks* a, VkSwapchainKHR* out) {
  // Streamline guide: DLSS-G off before any swapchain manipulation.
  if (g_genOn) {
    g_genOn = false;
    SlSetGeneration(false, g_genFrames);
  }
  g_validStreak = 0;
  OverlayReleaseSwapchain();  // our views/framebuffers of the old images go first
  VkResult r = vk.vkCreateSwapchainKHR(d, ci, a, out);
  if (r == VK_SUCCESS) {
    SetSwapchainInfo(ci->imageExtent, ci->imageFormat);
    OverlayOnSwapchainCreated(*out, ci->imageFormat, ci->imageExtent);
    Log("swapchain %ux%u format %d presentMode %d images>=%u", ci->imageExtent.width, ci->imageExtent.height,
        (int)ci->imageFormat, (int)ci->presentMode, ci->minImageCount);
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* imgs) {
  VkResult r = vk.vkGetSwapchainImagesKHR(d, sc, n, imgs);
  if (imgs && n && (r == VK_SUCCESS || r == VK_INCOMPLETE)) {
    SetSwapchainImages(imgs, *n);
    OverlayOnSwapchainImages(sc, imgs, *n);
  }
  return r;
}

PFN_vkVoidFunction FrameWrapper(const char* name) {
  if (!strcmp(name, "vkQueueSubmit")) return (PFN_vkVoidFunction)w_QueueSubmit;
  if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)w_QueuePresentKHR;
  if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)w_CreateSwapchainKHR;
  if (!strcmp(name, "vkGetSwapchainImagesKHR")) return (PFN_vkVoidFunction)w_GetSwapchainImagesKHR;
  return nullptr;
}
}  // namespace feverscaler
