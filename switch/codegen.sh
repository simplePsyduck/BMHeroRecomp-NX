#!/usr/bin/env bash
# Generates the recompiled C sources from your own Bomberman Hero (NTSC-U 1.0) ROM. Nothing derived from the ROM
# is tracked in git (RecompiledFuncs/, RecompiledPatches/, rsp/n_aspMain.cpp, bmhero.z64 are git-ignored).
# Runs inside ubuntu:24.04, from the repository root, with submodules checked out:
#   docker run --rm -v "$PWD:/work" -v "<rom dir>:/rom:ro" ubuntu:24.04 bash /work/switch/codegen.sh "/rom/<rom>.z64"
set -euo pipefail

ROM="$1"
PORT=/work
TOOLS=/work/switch/tools
ROM_SHA1=a36364b7e59351f7551ab351cb3b41ebc4be285b   # BUILDING.md: NTSC-U 1.0

apt-get -qq update
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y build-essential cmake ninja-build git clang lld llvm python3 >/dev/null

[ "$(sha1sum "$ROM" | cut -d' ' -f1)" = "$ROM_SHA1" ] || { echo "ROM is not Bomberman Hero NTSC-U 1.0 (sha1 $ROM_SHA1)" >&2; exit 1; }

# The recompilers come from the N64Recomp the repository's N64ModernRuntime pins (same version the game's
# patches were written against); built in the container on first run, then reused.
N64RECOMP_SRC="${N64RECOMP_SRC:-$PORT/lib/N64ModernRuntime/N64Recomp}"
if [ ! -x "$TOOLS/N64Recomp" ]; then
    [ -f "$N64RECOMP_SRC/CMakeLists.txt" ] || { echo "N64Recomp sources not found at $N64RECOMP_SRC (git submodule update --init --recursive)" >&2; exit 1; }
    mkdir -p "$TOOLS"
    cmake -S "$N64RECOMP_SRC" -B "$TOOLS/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$TOOLS/build" --target N64RecompCLI RSPRecomp
    cp "$TOOLS/build/N64Recomp" "$TOOLS/build/RSPRecomp" "$TOOLS/"
fi

cd "$PORT"
cp "$ROM" bmhero.z64                 # bmhero.toml and aspMain.toml read ./bmhero.z64
mkdir -p RecompiledFuncs RecompiledPatches rsp
"$TOOLS/N64Recomp" bmhero.toml
"$TOOLS/RSPRecomp" aspMain.toml
make -C patches CC=clang LD=ld.lld
"$TOOLS/N64Recomp" patches.toml

echo "Generated $(ls RecompiledFuncs | wc -l) files in RecompiledFuncs/"
