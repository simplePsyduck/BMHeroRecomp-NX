# Bomberman Hero (BMHero_nx) — Nintendo Switch port (Phase 1)

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
./switch/build.sh            # -> switch/build/BMHero_nx.nro
```
SD card: `sdmc:/switch/BMHero_nx/BMHero_nx.nro` + your ROM (`.z64`/`.n64`/`.v64`) next to it. Launch hbmenu in
title mode (hold R while starting a game).

## Licensing note
Switch layer files derived from DK64Recomp-NX are GPL-3.0, like BMHeroRecomp. GLideN64 is GPL-2.0-only, which is
not compatible with GPL-3.0 inside one distributed binary; building for your own use is unaffected.

## Pause menu
Press the menu hotkey (default **-**, rebindable) at any time the game is drawing. The game halts while it is open:
its thread stops at the next controller poll and the renderer keeps showing the last frame under the menu.
- **Continue** closes it. **Quit** exits the app. **Controllers** rebinds every N64 input and the hotkey (A rebinds,
  Y adds a second Switch input, X unbinds; the hotkey itself cannot be unbound); changes are written to `config.ini`
  as you make them. **Video Settings**: Render Quality (Native 240p, 480p, 720p, 1080p) and Aspect Ratio (16:9 or 4:3
  letterbox). Both are read once at startup: the menu marks them with * and "Apply Changes and Restart" relaunches
  the app through the loader (`envSetNextLoad`). **Mods** lists the mods in the mods folder and switches them on or
  off. **Reset Game** starts the game over (after a confirmation), which loads the mods as chosen.
- Render quality uses GLideN64's whole-number multiples of the N64's 240 lines, so there is no 360p step (1.5x).
- Build needs the `switch-glad` portlib (build.sh installs it) and Dear ImGui with its OpenGL 3 backend in
  `lib/rt64/src/contrib/imgui`, as DK64NX does.
- Unverified on hardware: that the game thread really stops at the controller poll (if the picture keeps moving
  behind the menu, the game polls input elsewhere), and the ImGui/GL state handoff with GLideN64 (copied from DK64NX).

## config.ini
Everything the menu changes is saved in one file, `sdmc:/switch/BMHero_nx/config.ini`: a `[settings]` section
(video, audio, performance, each key under a comment listing its values) and a `[controls]` section (the hotkey and
every N64 binding). The menu rewrites the whole file on each change; edit it by hand only while the game is closed.
On the first run with it, the older `nx_options.txt` and `controls.ini` are read once and carried over (they are left
in place and no longer read). Delete `config.ini` for the defaults.

## Telling builds apart
The first line of `BMHero_nx.log` carries the build's date and time. `build.sh` deletes the old `.nro` before it
starts and prints `BUILD OK` / `BUILD FAILED`: a failed build leaves nothing to copy by mistake.

## Button names
A, B, X and Y mean the Switch buttons as printed, everywhere: the menu (A accepts, B goes back), the binding list and
`config.ini`. switch-sdl2 reports the positional layout (SDL's A is the bottom button, the Switch's B), so
`input_switch.cpp` swaps A/B and X/Y when reading a controller. A `[controls]` section without the `LAYOUT = LABELS`
line was written under the old naming; it is converted once on load (A<->B, X<->Y) and rewritten, which keeps every
binding on the same physical button.

## Mods and Reset Game
Mod files (`.nrm`) go in `sdmc:/switch/BMHero_nx/mods/`. The Mods screen lists them with On/Off (A, or the D-pad,
flips one); a mod that another chosen mod depends on shows "On (needed)" and is turned on by the runtime. librecomp
loads mods when the game starts, so a change takes effect after a reset: choices are written to `mods.json` (next to
the mods folder, librecomp's own file, the other keys kept), and "Reset Game to Apply Changes" (or Reset Game on the
main menu) relaunches the whole app through the loader, which starts the game again with those mods. A relaunch, not
an in-process soft reset: the runtime cannot start a second game in the same process.
- Mods are compiled to native code at start by LiveRecomp's sljit through `jit_switch.c`. Unverified on hardware.
- `nx_mods.cpp` is the only file that calls the runtime's mod manager (`get_all_mod_details`, `is_mod_enabled`,
  `is_mod_auto_enabled`, `get_mods_directory`), with the names DK64Recomp-NX uses; if the runtime's `mods.hpp` differs,
  that file is the one to adjust.

## Extended render distance (not in the menu)
It was dropped from the menu: the chunk selector `func_800660DC` is compiled from assembly, and the limits that matter
(the per-level far plane `D_801779C8`, the 64 chunk slots, per-object 4:3 tests) are in the game's code. The hook it
used, `recomp_get_render_chunk_radius` (added to the six chunk ranges in `func_800663EC`), still returns 0, for a mod to
raise later. `bash switch/culling-excerpt.sh` collects the relevant game code.

## Names and the SD card layout
The file is `BMHero_nx.nro`; the launcher shows just "Bomberman Hero" (the NACP name, the window title and the menu
title). Everything the app needs on the SD card lives in one folder:
```
sd:/switch/BMHero_nx/
    BMHero_nx.nro        the app
    <your ROM>.z64       .z64 / .n64 / .v64, NTSC-U 1.0 (the app keeps its own checked copy as bmhero.z64)
    config.ini           settings and controls
    mods/                mod files (.nrm); mods.json next to it records which are on
    saves/               the game's save files
    BMHero_nx.log        the last run's log (BMHero_nx.prev.log: the run before)
```
The names and the folder are set in one place, `nx_paths.h`, and every file that needs a path uses it.

**Moving from the old folder.** Builds before this one used `sdmc:/switch/bmherorecomp/`. On the first run, the contents
of that folder (ROM, stored ROM copy, config, saves, mods) are moved into `sdmc:/switch/BMHero_nx/`; anything that
already exists in the new folder is left alone, and the old .nro and logs stay behind. Afterwards the old folder can
be deleted. Put the new `BMHero_nx.nro` in `sdmc:/switch/BMHero_nx/` (create the folder).
