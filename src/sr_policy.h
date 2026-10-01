#pragma once
#include <cstdint>

namespace feverscaler {
// When the DLSS preset may set the game's render resolution (present thread, once per frame).
// The preset's lower render size only makes sense while DLSS upscales it: until DLSS has produced
// a frame on this system the game keeps its own Resolution Scale, and if DLSS stops producing
// frames in the 3D view for good, SR is given up for the session. This includes startup failures:
// its temporary MSAA override must not last forever without an upscale. Menus (no 3D frame) do
// not count against DLSS.
struct SrScalePolicy {
  enum class Event { None, Proven, GaveUp };
  static constexpr uint32_t kFailFrames = 300;  // 3D frames without a DLSS image before giving up

  bool proven = false;   // DLSS produced a frame this session
  bool gaveUp = false;   // no more SR attempts or rendering overrides this session
  uint32_t failStreak = 0;

  // want: the user's SR switch and DLSS loaded; dlssFrame: DLSS made this frame's image;
  // frame3D: the frame had a 3D view DLSS should have upscaled.
  Event Update(bool want, bool dlssFrame, bool frame3D) {
    if (!want) {
      failStreak = 0;
      return Event::None;
    }
    if (dlssFrame) {
      failStreak = 0;
      if (proven || gaveUp) return Event::None;
      proven = true;
      return Event::Proven;
    }
    if (!frame3D || gaveUp) return Event::None;
    if (++failStreak < kFailFrames) return Event::None;
    gaveUp = true;
    return Event::GaveUp;
  }
  bool PresetScaleAllowed(bool want) const { return want && proven && !gaveUp; }
};
}  // namespace feverscaler
