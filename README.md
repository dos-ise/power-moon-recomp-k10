# Power Moon Recomp

*Super Mario Odyssey* running natively on PC.

[Power Moon Recomp](https://gitlab.com/zzzarathustra-dev/power-moon-recomp) by zzzarathustra-dev turns the Switch
game's ARM64 code into C ahead of time and compiles it into a regular PC program, with the Switch's system parts
(kernel, services, the NVN graphics API, audio) reimplemented on Vulkan.

> [!IMPORTANT]
> **You need your own copy of the game**, version **1.0.0** (no updates), dumped from your own console as `exefs` and
> `romfs`. Nothing of the game is in this repository or its releases. Instructions on dumping are not provided.

> [!NOTE]
> Like the original, this fork was made with an AI coding agent (Claude, through Claude Code), directed and tested on
> real hardware by a human.

## Play

1. Download the AppImage (Linux) or the Windows zip from this repository's
   [Releases](https://github.com/dos-ise/power-moon-recomp-k10/releases).
2. Linux: `chmod +x` the AppImage and start it. Windows: unzip the archive and run `PowerMoonRecomp.exe`. The first
   time, choose the folder with your game files (`exefs` and `romfs`). Setup checks the version, copies what the game
   needs into `~/.local/share/power-moon-recomp` (about 6 GB) and builds the game code on your PC for its CPU. Then
   the game starts; later starts go straight to it.
3. The in-game menu: **F1** or **Esc** (controller: Home / Guide). Details on the AppImage:
   [docs/linux.md](docs/linux.md).

Needs: glibc 2.35 or newer (Linux), a Vulkan 1.3 driver, 8 GB RAM (16 recommended), about 7 GB free disk space.

## What this fork changes

Everything not listed here is the original project, unchanged.

1. **Linux system font fallback.** The original only looked for DejaVu fonts, which Fedora Atomic (Bazzite) and other
   distributions don't ship, and aborted with `FATAL: no usable system font substitute`. The font loader now also
   scans the system font directories and picks any suitable TrueType font (`runtime/hle/pl.c`).
2. **No hang on a fatal error.** The fatal-error dialog on Linux waited forever for a click that never comes in a
   Steam/AppImage context; it now times out and the process exits (`runtime/platform/linux.c`).
3. **CI builds.** GitHub Actions builds the Linux AppImage and the Windows release zip on request, so both are
   published with every release.

## Thanks

To **zzzarathustra-dev** for Power Moon Recomp, to **Peiikai** and the contributors of
[LAS Recomp](https://github.com/peiikai/LASRecomp), which it is built on, and to **Revivedx**, **Igorms27** and the
Savage Wind Recomp contributors.

---

## About the game in this port

Everything below comes from the original project and applies here unchanged.

### Status

Playable from start to finish: the whole game has been played through without problems - every kingdom, the bosses,
the 2D and 8-bit sections, the ending and the post-game - at 60 fps on a current desktop PC (by the original's author).

Known issues:
- The first time an area appears, its shaders are compiled by the graphics driver. Shaders are compiled ahead, while
  the game loads, and side by side, but a stage you enter for the first time can still take a second or two to
  appear, and a new effect can hold the picture for a moment (the game and its sound keep running). Everything is
  cached afterwards (`cache/`): a second visit starts in a fraction of a second. Shaders the game has loaded but not
  shown yet are prepared at the next start, like emulators do: a small "Preparing shaders" window can appear for a few
  seconds before the game opens.
- Only version 1.0.0 of the game runs (no updates).
- The game runs at 60 fps, as on the Switch: its logic is tied to the frame rate (a higher rate would play faster).
  On 120 Hz+ displays, *Frame interpolation* (Video) shows 120 images per second; it adds about half a frame of latency.

### Controls

The in-game menu: **F1** or **Esc** (controller: Home / Guide button) - video, audio, controls (rebinding), language.

| Switch | Keyboard | Controller |
|---|---|---|
| Left stick | W A S D | left stick |
| Right stick (camera) | Num 8 / 4 / 5 / 6 | right stick |
| A / B / X / Y | K / J / L / I | by position (east = A, south = B) |
| L / R, ZL / ZR | Q / E, 1 / 3 | shoulder buttons / triggers |
| + / - | Enter / Backspace | Start / Back |
| Stick clicks | F / Num 0 | stick clicks |
| D-pad | arrow keys | d-pad |

Fullscreen: F11 or Alt+Enter.

### Files

Everything lives in one folder: on Windows the program's folder (a source checkout: the folder
`build\PowerMoonRecomp.exe` sits in, or its parent when that has `extracted\`), on Linux
`~/.local/share/power-moon-recomp`, on macOS `~/Library/Application Support/io.github.powermoonrecomp`:

- `extracted\` - your game files as setup made them (`exefs\`, `romfs.bin`) and the game code built from them
  (`odyssey_game.dll`)
- `save\` - your saves, `cache\` - compiled shaders, `powermoon.ini` - settings, `PowerMoonRecomp.log` - the log
- `mods\` - optional mods, each in a folder of its own (see below)
- `fonts\` - optional: the Switch's system fonts from your console (see `fonts\README.md`)

### Mods

Mods that replace or add game files work: costumes, models, textures, music, text, levels - RomFS mods, as made for
the Switch emulators. Put each mod in its own folder in `mods\` (in the folder above), with its `romfs` folder inside,
just as it comes: `mods\<Mod name>\romfs\ObjectData\...`. The Atmosphere layout
(`...\atmosphere\contents\0100000000010000\romfs\`) works too. The game's own files stay untouched.

In the menu (**F1**), the **Mods** tab lists them: switch each one on or off, or open the mods folder; changes apply the
next time the game starts. If two mods change the same file, the one higher in the list wins (the folder names in
alphabetical order).

Mods made for version 1.0.0 work best; ones made for later versions of the game may not. Code mods (exefs: IPS /
pchtxt patches, or code like Super Mario Odyssey: Online) can't be used - the game code is recompiled from the
original on your PC - and show up greyed out.

### How it works

- `tools/` - the recompiler (Python): `gen.py` lifts every function of the game's NSO modules to C (`lifter.py`),
  `odyssey_patches.py` holds the few game-specific instruction patches.
- `runtime/` - the program: module loader, a small Horizon kernel (threads, synchronization, memory; threads of one
  Switch core take turns as there), IPC services (file system, audio renderer, input, time, ...), HLE of SDK
  functions, the NVN graphics API on Vulkan with a Maxwell shader translator (`runtime/nvn`), movie playback with
  FFmpeg, the in-game menu (Dear ImGui), the first-start setup (`runtime/ui/setup.mm`, `runtime/gamebuild.c`).
- Debugging tools are environment variables (`ODYSSEY_*`), documented where they are read.

A release is made with `make deps-windows` (an LGPL-only FFmpeg), `make gen-helper` (the generator as a standalone
program), `tools/fetch_zig.sh` (the compiler setup uses on the player's PC) and `make dist` - the zip holds no game code.

### Credits

- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by Peiikai and its contributors - the static recompiler,
  runtime and renderer this port is built on, and the frame interpolation (MIT license). Huge thanks: without it this
  port wouldn't exist.
- **[OdysseyDecomp](https://github.com/MonsterDruide1/OdysseyDecomp)** - names and structure of the game's code, used
  for reading and debugging (no code from it is in this repository)
- [Switchbrew](https://switchbrew.org) and [envytools](https://github.com/envytools/envytools) - documentation of the
  Switch's system interfaces and the Maxwell GPU
- An MIT-licensed open-source project's audio renderer and shader instruction decoders (license and copyright notice:
  `third_party/ryujinx/LICENSE.txt`) - the audio effects follow its code, its decoders were the bit-layout reference
  for parts of the shader translator
- Libraries: [SDL 3](https://libsdl.org) (zlib), [FFmpeg](https://ffmpeg.org) (LGPL, linked dynamically),
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) (MIT),
  [shaderc](https://github.com/google/shaderc) (Apache 2.0), [SMAA](https://github.com/iryoku/smaa) (MIT,
  `third_party/smaa/LICENSE.txt`), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (MIT,
  `third_party/fsr/LICENSE.txt`),
  the [Roboto](https://github.com/googlefonts/roboto) font of the menu (Apache 2.0, from Dear ImGui's
  `third_party/imgui/misc/fonts`)
- In the release, to build the game code on your PC: [Zig](https://ziglang.org) (MIT) with its
  [LLVM / Clang / LLD](https://llvm.org) (Apache 2.0 with LLVM exception) and MinGW-w64 headers; the generator runs on
  [Python](https://www.python.org) (PSF license), frozen with [PyInstaller](https://pyinstaller.org) (GPL with
  bootloader exception), with [Capstone](https://www.capstone-engine.org) (BSD), [Unicorn](https://www.unicorn-engine.org)
  (GPL 2.0) and [LZ4](https://github.com/python-lz4/python-lz4) (BSD). Their sources are at those links; the release
  carries every licence in `licenses/` (with `licenses/README.txt`).

## Legal

This is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo. *Super Mario Odyssey* is a
trademark of Nintendo. No game files, game code, assets or fonts are included in this repository or its releases, and
nothing built from your game files (`extracted/`, `generated/`, `odyssey_game.dll`) may be shared. You must provide
your own legally obtained copy of the game.

## License

The code of this repository is under the [MIT License](LICENSE) (it keeps LAS Recomp's copyright notice; the fork's
additions from Savage Wind Recomp and TotK PC Recomp are MIT too and keep their notices). Third-party
components keep their own licenses (above, and in `third_party/`). The license covers this project's code only - it
grants nothing for Nintendo's game.
