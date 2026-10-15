#!/usr/bin/env bash
# Reads Quicksand-Bold.ttf (next to this file) and writes data/f_small.bcfnt, data/f_body.bcfnt and data/f_big.bcfnt.
# Needs mkbcfnt from devkitPro. Run it from the repo root: bash assets/fonts/make_fonts.sh
# The TTF is the Quicksand-Bold.ttf of Debian's fonts-quicksand package (SIL Open Font License, see OFL.txt).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
ttf="$here/Quicksand-Bold.ttf"
out="$here/../../data"
charset="$(mktemp)"
trap 'rm -f "$charset"' EXIT
# printable ASCII plus degree, middle dot, plus-minus, multiplication sign
{ seq 32 126; echo 176 183 177 215; } > "$charset"
mkdir -p "$out"
# mkbcfnt sizes come out about 1.7x larger in pixels than the number given.
mkbcfnt -s 6 -w "$charset" -o "$out/f_small.bcfnt" "$ttf"   # ~10 px: tile labels, hints
mkbcfnt -s 9 -w "$charset" -o "$out/f_body.bcfnt" "$ttf"    # ~15 px: values, menu rows
mkbcfnt -s 15 -w "$charset" -o "$out/f_big.bcfnt" "$ttf"    # ~26 px: dial readouts, timer
