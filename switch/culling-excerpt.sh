#!/usr/bin/env bash
# Collects the game's own code that decides what gets drawn (chunk selection, far plane, screen-edge checks) into
# culling-excerpt.txt in the repository root, so the Extended Render Distance option can be made to reach further.
#   bash switch/culling-excerpt.sh
cd "$(dirname "$0")/.."
D=lib/bmhero
out=culling-excerpt.txt
[ -d "$D/src" ] || { echo "no $D/src: run  git submodule update --init --recursive" >&2; exit 1; }
{
  echo "### chunk selector func_800660DC"
  grep -rn -A100 "func_800660DC(.*) {" "$D/src"
  echo; echo "### chunk table D_80104C70"
  grep -rn -B1 -A16 "D_80104C70\[" "$D/src" "$D/include" | head -80
  echo; echo "### position to chunk func_80065AEC"
  grep -rn -A60 "func_80065AEC(.*) {" "$D/src"
  echo; echo "### far plane D_801779C8"
  grep -rn "D_801779C8" "$D/src" "$D/include" | head -40
  echo; echo "### chunk slot arrays D_801777F0 / D_8017794C"
  grep -rn "D_801777F0\|D_8017794C" "$D/src" "$D/include" | head -40
  echo; echo "### 4:3 / screen-edge numbers"
  grep -rn "1\.3333\|320\.0f\|4\.0f / 3\.0f" "$D/src" | head -60
  echo; echo "### visibility words"
  grep -rni "frustum\|inview\|isvisible\|offscreen\|outside.*screen" "$D/src" | head -60
} > "$out" 2>&1
wc -c "$out"
echo "Upload $out"
