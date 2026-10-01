#pragma once
#include <cstdint>

namespace feverscaler {
// Render-resolution control independent of the game's Resolution Scale slider. The renderer's
// resize function (TF3 build 40408) is hooked: while an override is set, every window-size resize
// of the world view uses that scale, so the slider has no effect. Verified by code bytes; on
// another build the feature stays off.
bool GameScaleInit();       // after MinHook is initialised
bool GameScaleAvailable();
float GameSliderScale();    // the game's own Resolution Scale setting (0 if unknown)
// Scale for the next resizes (0 = the game's slider). A change makes the game resize.
void GameSetScaleOverride(float scale);
float GameScaleOverride();
void GameScalePoll();       // present thread, once per frame: drives a requested resize
}  // namespace feverscaler
