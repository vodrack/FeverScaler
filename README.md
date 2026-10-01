# FeverScaler

NVIDIA DLSS for Transport Fever 3. Beta.

- **DLSS Super Resolution and DLAA**: presets from DLAA to Ultra Performance, selectable DLSS model.
- **DLSS Frame Generation**: x2 to x6 depending on your graphics card, with NVIDIA Reflex.

## Requirements

- Transport Fever 3 on Windows
- NVIDIA GeForce RTX graphics card. Frame generation on RTX 20 and 30 series is experimental.

## Install

Close the game and extract the ZIP from [Releases](https://github.com/vodrack/FeverScaler/releases)
into the folder with `TransportFever3.exe`. If you already have a `winhttp.dll` from other mods,
see [Install and removal](docs/INSTALL.txt) first.

## Settings

Settings > Graphics > FeverScaler, or the FeverScaler menu (backslash key on US keyboards,
configurable).

## Compatibility

Not tested with other mods. Mods that change the game's shaders and tools that hook its
rendering (ReShade, other upscalers or frame generation) can conflict.

## Known issues

- Supports only game build 40408. Tested only on an RTX 3080.
- Occasional freezes at higher frame generation multipliers.
- Imperfect water reflections.
- Poor shadows, especially in the distance.
- Water, particles, construction previews, highlights, and objects that just appeared, move very
  fast or change level of detail can smear or ghost.

## Bug reports and feature requests

Both are welcome as [issues](https://github.com/vodrack/FeverScaler/issues). Bug reports need the
`feverscaler*.log` files from the game folder and `scripts\feverscaler.ini`.

## Building from source

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## License

FeverScaler's own code is [MIT](LICENSE). Bundled third-party components, including the NVIDIA
runtime, keep their own licenses: see [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
