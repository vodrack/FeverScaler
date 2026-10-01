#pragma once

namespace feverscaler {
// The game's VSync and MSAA settings while DLSS runs, without changing the saved settings: VSync
// off while DLSS-G is on, no MSAA while DLSS-G or DLSS Super Resolution is on. The render
// context's setters (TF3 build 40408) are hooked. Verified by code bytes; on another build the
// game's settings apply.
void GameSettingsInit();  // after MinHook is initialised
void GameSettingsPoll();  // present thread, once per frame: applies a change of the DLSS switches
}  // namespace feverscaler
