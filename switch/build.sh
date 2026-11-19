#!/usr/bin/env bash
# Builds switch/build/BMHero_nx.nro in the official devkitA64 container (needs Docker + git; works from Git Bash).
# Run switch/codegen.sh once first (it needs your own ROM).
#   BMHNX_RENDERER=deko3d ./switch/build.sh     # Phase 2 renderer, once switch/render_deko3d.cpp exists
set -euo pipefail
cd "$(dirname "$0")/.."

# A failed build leaves the previous .nro in switch/build, and copying that one is an easy mistake (the log then
# shows an old build). So: old outputs are deleted first, and a failure says so loudly.
rm -f switch/build/BMHero_nx.nro switch/build/BMHero_nx.elf
trap 'echo >&2; echo "BUILD FAILED: no new BMHero_nx.nro was produced. Do not copy an old one." >&2' ERR

# The pause menu is built from Dear ImGui in the rt64 submodule.
for f in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_opengl3.cpp; do
    [ -f "lib/rt64/src/contrib/imgui/$f" ] || {
        echo "missing lib/rt64/src/contrib/imgui/$f: git submodule update --init --recursive" >&2
        exit 1
    }
done

# The Switch changes to pinned submodules live in switch/patches, so the submodules stay on their upstream
# commits. Applying twice is a no-op.
apply_patch() {  # <submodule dir> <patch file>
    if [ ! -f "$2" ]; then
        echo "missing $2: rebase switch/patches/dk64-reference/$(basename "$2") onto this repo's pinned submodule" >&2
        echo "(switch/patches/try-reference.sh shows which hunks still apply)" >&2
        exit 1
    fi
    if git -C "$1" apply --reverse --check "$PWD/$2" 2>/dev/null; then
        return
    fi
    git -C "$1" apply "$PWD/$2"
}
apply_patch lib/N64ModernRuntime switch/patches/n64modernruntime.patch
apply_patch lib/N64ModernRuntime/N64Recomp switch/patches/n64recomp.patch
apply_patch lib/GLideN64 switch/patches/gliden64.patch

# DK64NX's GLideN64 patch registers the RT64 extended-GBI decoder (opcode 0x64) only for F3DEX2. Bomberman Hero
# runs F3DEX 1.21 ("RSP Gfx ucode F3DEX 1.21" in the ROM), so register it there too. Idempotent.
GBI=lib/GLideN64/src/GBI.cpp
if ! grep -q "BMHNX: BMHero uses F3DEX" "$GBI"; then
    if [ "$(grep -c 'F3DEX_Init();' "$GBI")" != 1 ]; then
        echo "expected exactly one F3DEX_Init(); in $GBI: add 'RT64EX_Init();' after it by hand, then re-run" >&2
        exit 1
    fi
    awk '{ print } /^[ \t]*F3DEX_Init\(\);/ { match($0, /^[ \t]*/); print substr($0, 1, RLENGTH) "RT64EX_Init(); // BMHNX: BMHero uses F3DEX 1.21" }' "$GBI" > "$GBI.tmp"
    mv "$GBI.tmp" "$GBI"
    echo "GBI.cpp: RT64EX_Init() added for F3DEX"
fi

[ -f patches/patches.bin ] || { echo "patches/patches.bin missing: run switch/codegen.sh first" >&2; exit 1; }

# Git Bash on Windows needs a Windows-style path for Docker and no path rewriting; elsewhere this is plain $PWD.
ROOT_MOUNT="$(pwd -W 2>/dev/null || pwd)"
export MSYS_NO_PATHCONV=1

RENDERER="${BMHNX_RENDERER:-gliden64}"
docker run --rm -e RENDERER="$RENDERER" -v "$ROOT_MOUNT:/work" -w /work/switch devkitpro/devkita64:latest bash -c '
    set -euo pipefail
    # Switch portlibs the build links (idempotent; the image only ships the compiler and libnx).
    dkp-pacman -Syu --noconfirm --needed switch-sdl2 switch-mesa switch-libdrm_nouveau switch-cmake switch-glad
    /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBMHNX_RENDERER="$RENDERER"
    cmake --build build -j"$(nproc)"
'
ls -la switch/build/BMHero_nx.nro
echo "BUILD OK: switch/build/BMHero_nx.nro, built $(date)"
