#!/usr/bin/env bash
# Dry-runs DK64Recomp-NX's patches against this repository's pinned submodules and lists which files
# apply cleanly and which hunks need a manual rebase. Changes nothing.
#   switch/patches/try-reference.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
check() {  # <submodule dir> <patch>
    echo "== $2 -> $1"
    git -C "$1" apply --check --verbose "$PWD/$2" 2>&1 | sed 's/^/   /'
}
check lib/N64ModernRuntime switch/patches/dk64-reference/n64modernruntime.patch
check lib/N64ModernRuntime/N64Recomp switch/patches/dk64-reference/n64recomp.patch
check lib/GLideN64 switch/patches/dk64-reference/gliden64.patch
