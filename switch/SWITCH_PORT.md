# Bomberman Hero: Recompiled — Nintendo Switch port (Phase 1)

Status: **scaffold, not yet built.** Written without the pinned submodule sources or a devkitPro toolchain, so
nothing here has been compiled against them. Treat the first build as the first test.

## Plan
1. **Phase 1 (this directory):** GLideN64 on OpenGL via mesa/nouveau, as DK64Recomp-NX does. Boots straight from
   your own ROM; no ROM-derived data is ever committed or shipped.
2. **Phase 2:** deko3d backend. `nx_renderer.h` / `renderer_select.cpp` already route everything through
   `RendererContext`; add `render_deko3d.cpp` and build with `BMHNX_RENDERER=deko3d`.

## Layout
| File | Role | Origin |
|---|---|---|
| `main_switch.cpp` | entry point, audio, game registration, positional `recomp::start` | DK64NX, adapted to BMHero's `main.cpp` API |
| `frontend_switch.cpp` | `recompui` stubs + `banjo::get_*` getters from `nx_settings` | new |
| `nx_settings.*` | options file `nx_options.txt` | DK64NX, trimmed to BMHero |
| `renderer_select.cpp`, `nx_renderer.h` | backend seam | new |
| `render_gliden64.cpp`, `core_switch.cpp`, `gliden64_rt64ex.cpp` | GLideN64 backend + RT64 extended-GBI decoder + interpolation | DK64NX, renamed, **unverified** |
| `input_switch.cpp`, `perf_switch.cpp`, `vsync_switch.cpp`, `window_switch.c`, `threads_switch.c`, `mman_switch.c`, `crash_switch.c`, `shim/` | libnx platform layer | DK64NX, renamed |
| `nx_menu.cpp`, `nx_menu_widgets.*`, `nx_controls.*` | pause menu (ImGui), control bindings | new; widgets from DK64NX |
| `patches/dk64-reference/` | DK64NX's patches to N64ModernRuntime, N64Recomp, GLideN64 | reference only |

Removed on purpose: DK64 cheats/dev tools/RAM view, DK64 "True frames" pacing (hard-coded DK64 RDRAM addresses),
and DK64's icon. `jit_switch.c` stays only so the patched `mods.cpp` links; mods are not loaded in Phase 1 (replaced by BMHero's own `icons/app.png`).

## What blocks a first build
1. **GLideN64** (done once cloned at the right commit). Not a BMHero submodule: `git clone https://github.com/gonetz/GLideN64 lib/GLideN64`, then check out
   the commit DK64NX's patch was written against. Its pre-image of `src/GBI.cpp` is blob `0423612`; find the commit with
   ```
   git -C lib/GLideN64 log --format=%H -- src/GBI.cpp | while read c; do
     [ "$(git -C lib/GLideN64 rev-parse --short=7 $c:src/GBI.cpp)" = 0423612 ] && echo $c; done
   ```
   then `git -C lib/GLideN64 checkout <that hash>` and re-run `switch/patches/try-reference.sh`.
2. **Patches.** Checked on 2026-11-14 with `git apply --check` against BMHero's pinned submodules: the
   N64ModernRuntime and N64Recomp patches apply cleanly (line offsets only), so they are installed as
   `patches/n64modernruntime.patch` and `patches/n64recomp.patch`. `patches/gliden64.patch` was installed after `git apply --check` also passed against GLideN64 at the
   matching commit, so `build.sh` finds all three.
3. **Runtime API drift** (checked against the uploaded `game.hpp`, `config.hpp`, `renderer_context.hpp`):
   `recomp::start` (positional), `GameEntry` field order, `GraphicsConfig` fields and enums, the `RendererContext`
   virtuals and `callbacks_t` all match what `main_switch.cpp` / `render_gliden64.cpp` use. Fixed from that check:
   `GAME_ID` is non-const (`is_rom_valid` / `select_rom` take `std::u8string&`) and the ROM error dialog goes
   through `recompui::message_box`. Not checked yet: `ultramodern.hpp` callback structs (`audio_callbacks_t`,
   `input::callbacks_t`, `events::callbacks_t`, `threads::callbacks_t`), which `main.cpp` of BMHero uses by the
   same names.
4. **RT64 extended GBI** (checked against the uploaded `rt64_extended_gbi.h`): all 50 sub-opcodes (0x02 to 0x33)
   have exactly the command lengths in `COMMAND_LENGTH`, so display lists parse correctly. `gEXEnable` /
   `gEXDisable` are RT64 hook no-ops (`G_SPNOOP` with the `RT64` magic), not extended opcodes. The game's
   microcode is **F3DEX 1.21**, but DK64NX's GLideN64 patch registers the 0x64 decoder only for F3DEX2:
   `build.sh` adds `RT64EX_Init()` to the F3DEX case (idempotent). The F3DEX2-only patch hunk (segment masking)
   matters only with extended RDRAM, which BMHero's patches do not enable.
5. `src/game/config.cpp` (RmlUi menus) is replaced by `frontend_switch.cpp`.

## Build
```
git clone --recurse-submodules <BMHeroRecomp>  &&  add GLideN64 at lib/GLideN64
docker run --rm -v "$PWD:/work" -v "<rom dir>:/rom:ro" ubuntu:24.04 bash /work/switch/codegen.sh "/rom/<rom>.z64"
./switch/build.sh            # -> switch/build/bmherorecomp.nro
```
SD card: `sdmc:/switch/bmherorecomp/bmherorecomp.nro` + your ROM (`.z64`/`.n64`/`.v64`) next to it. Launch hbmenu in
title mode (hold R while starting a game).

## Licensing note
Switch layer files derived from DK64Recomp-NX are GPL-3.0, like BMHeroRecomp. GLideN64 is GPL-2.0-only, which is
not compatible with GPL-3.0 inside one distributed binary; building for your own use is unaffected.

## Pause menu
Press the menu hotkey (default **-**, rebindable) at any time the game is drawing. The game halts while it is open:
its thread stops at the next controller poll and the renderer keeps showing the last frame under the menu.
- **Continue** closes it. **Quit** exits the app. **Controllers** rebinds every N64 input and the hotkey (A rebinds,
  Y adds a second Switch input, X unbinds; the hotkey itself cannot be unbound); changes are written to `config.ini`
  as you make them. **Video Settings**: Render Quality (Native 240p, 480p, 720p, 1080p), Aspect Ratio (16:9 or 4:3
  letterbox) and Extended Render Distance (0 to +3 chunks, applied live through `recomp_get_render_chunk_radius`,
  which `func_800663EC` adds to all six chunk-range limits). Quality and aspect are read once at startup: the menu
  marks them with * and "Apply Changes and Restart" relaunches the app through the loader (`envSetNextLoad`).
- Render quality uses GLideN64's whole-number multiples of the N64's 240 lines, so there is no 360p step (1.5x).
- Build needs the `switch-glad` portlib (build.sh installs it) and Dear ImGui with its OpenGL 3 backend in
  `lib/rt64/src/contrib/imgui`, as DK64NX does.
- Unverified on hardware: that the game thread really stops at the controller poll (if the picture keeps moving
  behind the menu, the game polls input elsewhere), and the ImGui/GL state handoff with GLideN64 (copied from DK64NX).

## config.ini
Everything the menu changes is saved in one file, `sdmc:/switch/bmherorecomp/config.ini`: a `[settings]` section
(video, audio, performance, each key under a comment listing its values) and a `[controls]` section (the hotkey and
every N64 binding). The menu rewrites the whole file on each change; edit it by hand only while the game is closed.
On the first run with it, the older `nx_options.txt` and `controls.ini` are read once and carried over (they are left
in place and no longer read). Delete `config.ini` for the defaults.

## Telling builds apart
The first line of `bmherorecomp.log` carries the build's date and time. `build.sh` deletes the old `.nro` before it
starts and prints `BUILD OK` / `BUILD FAILED`: a failed build leaves nothing to copy by mistake.

## Button names
A, B, X and Y mean the Switch buttons as printed, everywhere: the menu (A accepts, B goes back), the binding list and
`config.ini`. switch-sdl2 reports the positional layout (SDL's A is the bottom button, the Switch's B), so
`input_switch.cpp` swaps A/B and X/Y when reading a controller. A `[controls]` section without the `LAYOUT = LABELS`
line was written under the old naming; it is converted once on load (A<->B, X<->Y) and rewritten, which keeps every
binding on the same physical button.

## Render distance: what the option can and cannot move
Extended Render Distance adds to the six chunk ranges in `func_800663EC`. Other limits it does not touch: the
projection's far plane (`guPerspective(..., 100.0f, D_801779C8.raw, ...)` in `func_800821E0`, a per-level value), the
fixed 64 slots of the chunk list (`D_801777F0[0x40]`, objects 0x4E to 0x8E), and any per-object test against the
original 4:3 view. `bash switch/culling-excerpt.sh` collects the game code for these into `culling-excerpt.txt`.
