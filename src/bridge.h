#pragma once
#include <string>

namespace feverscaler {
// The plugin's group in the game's Settings > Graphics page: a game script (settings/, installed
// to scripts\feverscaler\settings\) that the plugin serves and talks to.
//   - The game runs a mod's scripts only in saves with that mod enabled, but base-game content in
//     the main menu and every save. So the script is served as base content: the game's opens of
//     base\_content.json (its list of base content) get a copy with the script's files added, and
//     its file access under base\content\feverscaler\ goes to scripts\feverscaler\settings\.
//   - Game scripts cannot call native code; they read and write Lua tables in the game's user data
//     folder. <user data>\feverscaler\state.lua mirrors the switches. The plugin rewrites it when
//     they change; the script rewrites it when the user changes one in the settings page.

// DllMain, only with the plugin enabled.
void BridgeInit();
// Present thread, once per frame: applies changes from the script, publishes the plugin's state.
void BridgePoll();
// The game's user data folder (the one holding its settings.lua) with a trailing backslash, or
// empty while the game has not opened its settings yet.
std::wstring BridgeUserDataDir();
}  // namespace feverscaler
