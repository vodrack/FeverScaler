#include <windows.h>

#include "bridge.h"
#include "config.h"
#include "diagnostics.h"
#include "game_scale.h"
#include "game_settings.h"
#include "hooks.h"
#include "log.h"
#include "feverscaler_version.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    feverscaler::LogInit();
    feverscaler::Log("FeverScaler " FEVERSCALER_VERSION " attach (build " __DATE__ " " __TIME__ ")");
    feverscaler::LoadConfig();
    if (feverscaler::Cfg().enabled) {
      feverscaler::DiagnosticsInit();
      feverscaler::BridgeInit();
      feverscaler::InstallLoaderHooks();
      // The DLSS preset sets the render resolution only once DLSS is known to upscale it (frame.cpp).
      feverscaler::GameScaleInit();
      feverscaler::GameSettingsInit();
    }
    else feverscaler::Log("Enabled=0 - plugin inactive");
  }
  return TRUE;
}
