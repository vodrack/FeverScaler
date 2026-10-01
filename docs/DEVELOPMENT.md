# FeverScaler development notes

## How it works

FeverScaler is an ASI plugin that adds upscaling and frame generation to Transport Fever 3 (Vulkan).
DLSS Super Resolution and DLSS Frame Generation are implemented; FSR and XeSS are planned.

The game has no temporal upscaler, motion vectors or jitter, so the plugin builds these inputs itself:

- **Camera motion** for every pixel, reprojected from the scene depth with the current and previous
  camera matrices (read from the game's `u_view` uniform buffer, which the CPU writes into mapped memory).
- **Object motion** for moving vehicles and skinned meshes (people, animals): every frame the plugin
  reads each instanced draw's model matrices (and each skinned draw's joint matrices) from mapped
  memory, diffs them against the previous frame, and re-draws only the draws that moved into the
  motion-vector buffer with its own shaders, depth-tested against the game's depth. No game shaders
  or render passes are modified.
  Rigid instances are pooled by geometry across draw batches and matched one-to-one to the previous
  frame by position, within 5 metres per frame, so draw order, culling and instance count do not
  affect the match. Missing or ambiguous history falls back to camera motion.
- **HUD-less colour**: a copy of the finished scene image taken just before the UI pass.

Streamline 2.12.0 / DLSS-G 310.7 does the frame generation; Reflex markers are placed around the
game's submits and present. On RTX 20/30 cards, RTX30MFG-Unlock lets DLSS-G run.

Everything ships in one drop-in ZIP for the game folder. The plugin (`src/`) also puts its switches
into the game's own Settings > Graphics page, through a game script (`settings/`) that it serves to
the game itself; see [Settings page](#settings-page).

## Build

Requirements: Visual Studio 2022 Build Tools, CMake 3.25+, Ninja, Git and the Vulkan SDK (for
`glslc` and headers; its installer sets `VULKAN_SDK`). Everything else (Streamline SDK, MinHook,
Dear ImGui, Ultimate ASI Loader, RTX30MFG-Unlock) is downloaded into `build/_deps`
on the first configure, pinned by URL and SHA-256 in [CMakeLists.txt](../CMakeLists.txt). The unlock
is built from source with
[`patches/rtx30mfg-compact-logging.patch`](../patches/rtx30mfg-compact-logging.patch), which changes
only its disk logging.

Run these in a Visual Studio x64 developer shell. From a plain shell, prefix them with the build
environment: `cmd /c '"<VS>\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake ...'`.

```
cmake --workflow --preset default   # configure, build, test, and package into build\packages\
cmake --build build                 # rebuild after a change
cmake --install build --prefix "<game folder>"
                                    # the drop-in layout, as in the ZIP
```

To remove an installed plugin, delete the files listed in [docs/INSTALL.txt](INSTALL.txt).

## Tests

None of the tests launch the game. The workflow preset runs the C++ tests; on their own they run
with `ctest --preset default` (one test: `ctest --preset default -R motion_history`). After the
package step, `powershell -NoProfile -File tests/package_test.ps1` checks the ZIP in
`build/packages`. `tests/settings_test.lua` runs the settings script against a simulated plugin and
game; run it with Lua 5.4 from the repository root.

## CI and releases

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on every push to `main`: the workflow
preset and the package check on Windows, the settings test on Linux. The run keeps the ZIP as its
`packages` artifact and `feverscaler.map` as its `map` artifact. Pushing a tag `v<VERSION>` (for
example `v0.1.0-beta`) also publishes a GitHub release with the ZIP and `SHA256SUMS.txt`, with
GitHub's generated release notes, marked as the latest release. The release fails if the tag does
not match `VERSION`.

## Diagnostics

The plugin writes these files beside `TransportFever3.exe`:

| Files | Maximum size per file | Contents |
|---|---:|---|
| `feverscaler.log`, `feverscaler.log.previous` | 512 KiB | Startup, system, modules, settings, periodic rendering and frame-time statistics and recent activity |
| `feverscaler-events.log`, `feverscaler-events.log.previous` | 256 KiB | Exceptions, Streamline warnings/errors, API failures and slow/stalled calls with current settings |
| `feverscaler-unlock.log`, `feverscaler-unlock.log.previous` | 512 KiB | Unlock initialization, provider/driver details and compatibility decisions |

Together they take at most **2.5 MiB**. The plugin logs rotate at the size limit and on restart.
The unlock log stops at its limit and keeps the previous session on restart.

Repeated events from the same call site are summarized every 30 seconds with a count and the latest
message. A sampler checks outstanding present, submit, GPU-fence, DLSS and Reflex calls every
250 ms and records calls still running after 2 seconds, with the stalled thread's stack, and calls
that return after 500 ms. The present call spans the plugin's whole present wrapper, including the
menu, settings polling and the frame generation gate. Loading, debugger pauses and some setting
changes can also trigger it; idle time and periods with frame generation off do not. The sampler
suspends a stalled thread only while it copies that thread's registers and stack, walks the copy
afterwards and calls no graphics APIs. There are no process dumps or uploads.

System exceptions (codes `0xC…`: access violations, illegal instructions, stack overflows) are
logged with their stack when raised, once per faulting address every 30 seconds. The game or a
driver may still handle one; a crash is the last exception before the log ends. Fail-fast
terminations bypass the handler and leave no record. Stacks are `module+0xoffset` frames, innermost
first. `feverscaler.asi` offsets are RVAs in that build's `build/feverscaler.map`.

At startup the log lists the modules loaded from outside the Windows folder (the game's libraries,
overlays, capture hooks, Vulkan layers, other mods), then logs each such module as it loads. The
environment line adds the Windows build, CPU, thread count and RAM to the GPU and driver. Every 600
presents the log records the game's frame times before frame generation (average, 99th percentile,
longest) and VRAM use against the Windows budget.

Streamline's own log file is off; its warning/error callback feeds the event log at any
`SlLogLevel`. `SlLogLevel=2` adds informational messages to the activity log. External
NVIDIA/Streamline log overrides (environment variables, registry settings, or the unlock's debug
marker file) write separate vendor logs outside these limits.

The unlock writes no per-frame CSV by default; its live counters keep running. With
`FEVERSCALER_UNLOCK_TRACE=1` set before launch it writes `feverscaler-unlock-trace.csv` beside the
executable, up to 1 MiB, overwritten on the next traced launch.

What to attach to a bug report: [docs/diagnostics.txt](diagnostics.txt) (shipped as
`scripts/feverscaler/DIAGNOSTICS.txt`).

## In game

- **VSync and MSAA** (`src/game_settings.cpp`): while DLSS-G is on, the game's VSync is off. On
  Vulkan DLSS-G does not support VSync: with a FIFO swapchain it shows few or no generated frames
  (G-SYNC/FreeSync work). While DLSS-G or SR is on, the game's MSAA is off: the plugin builds the
  DLSS inputs from a copy of the scene depth, which `vkCmdCopyImage` cannot take from a
  multisampled image. The plugin hooks the two setters the game hands these settings to its
  render context through (build 40408 only, checked by code bytes; other builds keep the game's
  settings). A read-only context query captures the initial values too, since startup initializes
  them directly without calling those setters. The saved settings do not change and apply again
  when both switches are off. A switch change repeats the game's last calls on the window thread;
  the game then recreates its swapchain or rebuilds its render passes and pipelines. The log shows
  `game VSync ...` / `game MSAA ...`.
- In the background TF3 throttles itself to about 30 fps, where DLSS-G generates no frames (the log
  shows `x1.0`–`x1.3` instead of `x2.00`).
- The plugin follows the game's render size. With a resolution scale below 100 % the HUD-less image
  is the render-resolution scene stretched to the screen, the same way the game's UI pass
  stretches it.
- DLSS-G switches on after 60 frames of 3D rendering and suspends itself in menus/loading screens.
- **DLSS Super Resolution** (on by default, `SuperResolution=1`): the render resolution comes from
  the DLSS preset (`DlssMode`): DLAA 100 %, Quality 66.7 %, Balanced 58 %, Performance 50 %,
  Ultra Performance 33.3 %. The preset takes over once DLSS has produced its first frame; until
  then, and on PCs where DLSS cannot run, the game's own Resolution Scale applies. The plugin hooks
  the game's renderer resize (build 40408 only, checked by code bytes; other builds keep the game's
  slider): while the preset applies, every window-size resize of the world view, including the one
  that applies changed graphics settings, gets the preset's scale, so the Resolution Scale slider
  has no effect; the settings page shows it disabled while DLSS Super Resolution is on and usable.
  The saved slider value does not change. DLSS replaces the game's FSR1 upscale/sharpen/film grain.
  The DLSS model is chosen in the dev menu (`DlssPreset`): CNN (E), Transformer (DLSS 4, K),
  Transformer 2 (DLSS 4.5, L, default) or Transformer 2 fast (M).
- The 3D previews in vehicle / station windows are not upscaled by DLSS. While a preset is picked
  they render at `PreviewScale`: 0 full resolution, 1 the preset's render scale (default). It is
  also in the dev menu and applies the next time a preview window opens.
- Screen-space reflections, while DLSS runs: the game's checkerboard ray offset (which DLSS would
  keep as a dot pattern) is off. The ray step grows with the render height (x1 at 480 lines), so
  reflections reach as far at DLAA as at Ultra Performance (in the unmodded game they get cut off
  at high render resolutions). Instead of the checkerboard, the pixels of each 2x2 block start
  their steps at 0, 1/4, 1/2 and 3/4 of a step, rotated every frame by the DLSS jitter, so thin
  features the long steps skip average out in DLSS instead of leaving horizontal strips.
- **Texture detail with DLSS** (`src/mip_bias.cpp`): TF3's samplers have a fixed LOD bias, so below
  100 % render resolution the GPU samples the mips for the render size, and DLSS cannot restore that
  detail. While DLSS upscales, world-pass binds use plugin copies of the game's descriptor sets in
  which the sampler of each mip-mapped asset texture is a twin with an extra bias of
  log2(render width / output width): -0.59 at Quality, -1.58 at Ultra Performance. Render targets
  (environment maps, the HDR blur chain) and compare samplers keep the game's samplers, because in
  Vulkan the sampler bias also shifts explicit-LOD reads. Separate sampler descriptors, which TF3
  uses only for the terrain and grass texture arrays, are biased. A copy is freed 8 presents after
  the game rewrites or releases its set. Texture streaming already loads mips for the output
  resolution: it sizes by the window height, not the render height.
- **Foliage with DLSS:** TF3 draws foliage and grass with alpha-to-coverage, which without MSAA is a
  fixed dot pattern that DLSS keeps. While SR is on, those pipelines are swapped for copies without
  it whose fragment shader does a hashed alpha test instead (world-position hash on a quarter-pixel
  grid; DLSS's jitter varies it per frame and DLSS averages it into soft edges). `HashedAlpha=0`
  uses solid cut-outs instead, `AlphaToCoverage=1` keeps the game's own behaviour.
- **Settings > Graphics > FeverScaler**: DLSS Super Resolution, DLSS Quality (the render
  resolution preset), DLSS Model, Preview Resolution, DLSS Frame Generation, multiplier and the dev
  menu key. See [Settings page](#settings-page).
- **Frame generation options:** Multipliers x2 through x6, limited to what the runtime reports
  for the GPU and driver. Native multi frame generation is an RTX 50 feature; use on older cards
  through RTX30MFG-Unlock is experimental.
- **Dev menu**, shown and hidden with the backslash key on US keyboards: the same switches, plus
  the status and the rendered vs. shown frame rate. Its key label follows the Windows layout.
  The mouse works on the menu; clicks elsewhere still reach the game. The key is the plugin's only
  one and acts only while the game window is in the foreground. In the settings page it is set like
  the game's own key bindings: click the button, then press a key, optionally with Ctrl, Shift or
  Alt; Esc cancels. (TF3 binds Insert, Home, End, Delete, Page Up/Down and F1–F11.)
- Whatever is chosen in either place is saved to the ini, including frame generation on/off
  (`FrameGeneration`).
- Settings: `scripts\feverscaler.ini` (written on first run; missing keys take their defaults).
  `GeneratedFrames` 1–5 = x2–x6. Legacy `FrameGenerationMode` and `DynamicTargetFPS` keys are ignored.
  `KeyMenu` takes a virtual-key code (`0xDC`) or a character, optionally with modifiers:
  `KeyMenu=Ctrl+Shift+F`. `0` means no key.
- Logs: `FG stats: ... (x2.00)` in `feverscaler.log` means generated frames are being shown.

## Settings page

The plugin adds a "FeverScaler" group to the Graphics tab of the game's settings page, below
"Window Settings", in the main menu and in every save.

- **How the script gets into the game** (`src/bridge.cpp`). The group comes from a game script,
  `settings/` (installed to `scripts\feverscaler\settings\`). The game runs a mod's scripts only in
  saves with that mod enabled, but base-game content everywhere, so the plugin serves the script as
  base content. Base content is `base\content`, listed in `base\_content.json`. At every start the
  plugin writes a copy of that list with the script's files added
  (`scripts\feverscaler\base_content.json`) and hooks the game's file access (`CreateFileW`,
  `GetFileAttributesW`, `GetFileAttributesExW`, `FindFirstFileW`): reads of the list get the copy,
  and `base\content\feverscaler\` is `scripts\feverscaler\settings\`. Nothing in the game's own
  folders changes. The log shows `bridge: settings page served` and the first redirected file
  accesses. With `Enabled=0` nothing is served and the page is the game's own.
- **How it gets into the page.** The settings page builds its tabs inside one local function and has
  no extension point. The script loads a second instance of the game's `settings_page.tl` whose
  `ipairs` adds the group to the Graphics tab, and registers it through the game's recipe replacement
  (`settings_page.res.lua`, a `react-replacement-config`). The page stays the game's own code. If a
  game update changes what the script relies on, it logs a warning and the page is the unmodified one.
- **The menu key's button.** The page's option types bind keys only for the game's own actions. The
  script's page instance therefore also gets a `builtin` whose `TextView` turns the "FeverScaler
  Menu Key" label into the label plus a key button, styled like the game's key binding rows, with
  the key named by the game (`api.type.KeyCombo`). The button reports SDL scancodes (physical keys);
  the plugin turns them into virtual keys for the current keyboard layout (`KeyFromScancode` in
  `src/config.cpp`). Numpad digits bind as their Num Lock on keys. The button recipe returns a
  BoxLayout containing the button, as the game's recipe transformation requires. Its key listener
  stays registered while idle, passing those events through: the game installs keyboard routing
  when the widget is first created.
- **How it talks to the plugin.** Game scripts cannot call native code; they can read and write Lua
  tables in the game's user data folder (the one with its `settings.lua`). The plugin mirrors its
  switches to `<user data>\feverscaler\state.lua`. The script reads that file when the page is built
  and rewrites it when a switch changes; the plugin checks it ten times a second, applies what
  changed and writes back what it made of it. The plugin learns the user data folder from the
  game's own file access (the folder of the `settings.lua` the game opens). Script and plugin ship
  together, so the file has no version.
- A feature this PC cannot run shows the plugin's reason (for example an old driver or a non-RTX
  GPU) as a short row label with the full reason in the game's help panel, and its controls are
  disabled.

## Layout

| file | role |
|---|---|
| `src/hooks.cpp` | detours `vulkan-1!vkGetInstanceProcAddr`, routes the game through the Streamline interposer (model from bg3fgvk, MIT) |
| `src/sl_bridge.cpp` | `slInit`, feature functions, frame tokens + PCL markers, tags, constants, DLSS-G options |
| `src/tracker.cpp` | pipeline classification from SPIR-V names, mapped memory, descriptor writes, per-command-buffer draw recording |
| `src/frame.cpp` | per-frame logic: instance/joint diffing, pre-pass (depth copy, camera MV, object replay), HUD-less copy, gate |
| `src/gpu.cpp` | the plugin's Vulkan resources and pipelines |
| `shaders/` | camera motion (compute) and replay shaders (mirror the game's vertex math) |
| DLSS SR (in `frame.cpp`) | jitters the camera block before submit, runs DLSS on the post-compose scene copy at the UI pass, hands the result to that UI pass only (see [Rendering failure handling](#rendering-failure-handling)) |
| `src/mip_bias.cpp` | copies of the game's descriptor sets: biased samplers for the world pass while DLSS upscales, the DLSS output in place of the scene image for the UI pass |
| `src/overlay.cpp` | dev menu: Dear ImGui drawn into the presented image, WndProc subclass for mouse input |
| `src/bridge.cpp` | serves the settings script to the game; its state file |
| `settings/` | the settings script and the resource file that registers it |

## Rendering failure handling

With both DLSS switches off, submits skip the pre-pass, motion processing and tags; the HUD-less
copy is off too. Depth and colour copies require tracked single-sample images and transfer-source
usage. Multisampled depth suspends DLSS-G immediately and never reaches the depth copy.
The UI-recording guard uses the most recently recorded world depth, since the game's UI command
buffer can be recorded before its world command buffer is submitted. Destroying that image clears
the recorded handle.
If accumulated frame data is reset before presentation, the camera is recovered from the original
matrices even when its buffer was already jittered in that frame; the jitter is not applied twice.

Only the UI pass samples the DLSS output: after DLSS evaluates in a command buffer, the sets that
pass binds with the scene image it stretches onto the screen are swapped for copies holding the
output. The game's sets keep its image. Without FSR1, which the game skips with the Resolution
Scale above 95 %, and at 100 % render scale, that image is also the tonemapper's input; fed the
output, the tonemapper re-tonemaps it every frame and the scene turns grey. A frame counts as a
DLSS frame only once a submitted UI pass bound the output; without one the UI pass shows the
game's picture.

Foliage and reflection pipeline copies and mip-biased descriptor set copies apply only after SR
produces an image, and only to the world view. The game's original shaders, descriptor sets and
samplers stay available for fallback. After 300 world frames without a DLSS frame, including a
startup that never succeeds, SR stops for the session and releases its resolution and MSAA
overrides. MSAA remains off if FG still needs it.
The dev menu and the settings page report the SR failure. Restart the game to retry.

World-view detection uses the window size and known render scale. On unsupported builds it reads
the saved Resolution Scale from the actual user-data folder once per second. Unknown sizes are
rejected; a vehicle/station preview cannot become the world merely because the hook is absent.

## Known limits

- Particles and water surface animation carry camera motion only.
- Water reflection quality is imperfect.
- Object motion needs usable previous-frame history. Rigid instances use one-to-one spatial
  matching, skinned meshes nearest-root matching, both with conservative motion bounds. New,
  ambiguous, changing-LOD or very fast objects fall back to camera-only motion.
- World overlays drawn after tonemapping (construction previews, highlights) are part of the HUD-less
  image and have camera motion only.
- Intermittent freezes can occur at higher frame generation multipliers.
