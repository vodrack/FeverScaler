// The render-resolution hooks with fake game functions: no game, no hook installation.
#include "../src/game_scale.cpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace feverscaler {
void Log(const char*, ...) {}
const Config& Cfg() { static Config c; return c; }
float ReadGameResolutionScale(const wchar_t*) { return 0; }
std::wstring BridgeUserDataDir() { return {}; }
VkExtent2D SwapchainExtent() { return {3440, 1441}; }
uint64_t LastWorldPassTick() { return GetTickCount64(); }
unsigned long OverlayWindowThread() { return GetCurrentThreadId(); }
bool OverlayRunOnWindowThread(void (*fn)()) { fn(); return true; }
}  // namespace feverscaler

using namespace feverscaler;

// What reached the game: resizes and FSR1 settings, in order.
struct Event {
  void* renderer;
  bool fsr;
  ViewConfig cfg;
  FsrSettings settings;
};
static std::vector<Event> events;
static void* const world = (void*)0x1000;
static void* const other = (void*)0x2000;
static FsrSettings worldFsr{1, 1.0f, 0.0f, 1.0f, 0.3f}, otherFsr{};

static void Resize(void* r, ViewConfig c) {
  h_Resize(r, &c);
}
static const Event& Last() { return events.back(); }

int main() {
  o_Resize = [](void* r, ViewConfig* c) { events.push_back({r, false, *c, {}}); };
  o_SetFsr = [](void* r, const FsrSettings* s) {
    (r == world ? worldFsr : otherFsr) = *s;
    events.push_back({r, true, {}, *s});
  };
  o_GetFsr = [](void* r) -> const FsrSettings* { return r == world ? &worldFsr : &otherFsr; };
  g_available = g_fsrControl = true;
  g_main = Call{world, {3440, 1441, 0, 0.9f}, true};  // the window-size callers have sized the world view

  // Without a preset the game's own resizes and FSR1 settings pass unchanged.
  Resize(world, {3440, 1441, 0, 0.9f});  // applying graphics settings: stored size, new slider
  CHECK(Last().cfg.scale == 0.9f && worldFsr.enabled == 1);

  // With a preset every window-size resize of the world view gets its scale, whoever calls it, and
  // FSR1 is off before the resize. The size stays the game's: the next settings apply multiplies it.
  GameSetScaleOverride(2.0f / 3.0f);
  events.clear();
  Resize(world, {3440, 1441, 0, 0.95f});
  CHECK(events.size() == 2 && events[0].fsr && events[0].settings.enabled == 0 && events[0].settings.sharpening == 1.0f);
  CHECK(!events[1].fsr && events[1].cfg.width == 3440 && events[1].cfg.height == 1441 && events[1].cfg.scale == 2.0f / 3.0f);
  Resize(world, {3440, 1441, 0, 1.0f});  // back from a screenshot
  CHECK(Last().cfg.scale == 2.0f / 3.0f && worldFsr.enabled == 0);
  // Other sizes are the game's business: 0x0 and back at the render size, screenshots, other views.
  for (ViewConfig c : {ViewConfig{0, 0, 0, 1.0f}, ViewConfig{2293, 961, 0, 1.0f}, ViewConfig{6880, 2882, 0, 1.0f}}) {
    Resize(world, c);
    CHECK(Last().cfg.width == c.width && Last().cfg.height == c.height && Last().cfg.scale == c.scale);
  }
  Resize(other, {3440, 1441, 0, 0.9f});
  CHECK(Last().renderer == other && Last().cfg.scale == 0.9f);

  // The game's own FSR1 changes while the preset applies are kept for later and stay off; readers
  // of its settings (copies to other renderers, its editor) see the game's.
  FsrSettings game{1, 0.5f, 0.0f, 1.0f, 0.3f};
  h_SetFsr(world, &game);
  CHECK(worldFsr.enabled == 0 && worldFsr.sharpening == 0.5f);
  CHECK(h_GetFsr(world)->enabled == 1 && h_GetFsr(world)->sharpening == 0.5f);
  h_SetFsr(other, h_GetFsr(world));
  CHECK(otherFsr.enabled == 1);
  CHECK(h_GetFsr(other) == &otherFsr);

  // A preset change resizes on the window thread; without a preset the game's last window-size
  // resize and its FSR1 settings come back.
  GameScalePoll();
  CHECK(Last().cfg.scale == 2.0f / 3.0f);
  GameSetScaleOverride(0.0f);
  events.clear();
  GameScalePoll();
  CHECK(events.size() == 2 && events[0].fsr && events[0].settings.enabled == 1 && events[0].settings.sharpening == 0.5f);
  CHECK(events[1].cfg.width == 3440 && events[1].cfg.scale == 1.0f);  // the last one came back from a screenshot
  CHECK(h_GetFsr(world) == &worldFsr);
  game.enabled = 0;
  h_SetFsr(world, &game);
  CHECK(worldFsr.enabled == 0);
  std::puts("PASS: preset scale for every window-size resize of the world view, other resizes untouched, FSR1 off "
            "while the preset applies and the game's settings kept and restored");
}
