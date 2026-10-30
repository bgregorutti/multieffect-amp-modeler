#!/usr/bin/env bash
# Rasterize the component images (control-daemon/data/img/*.svg) into the
# PNGs the display client draws. Pillow cannot read SVG, and these use
# gradients and filters, so they are rendered once here with librsvg
# (apt install librsvg2-bin) and the PNGs are committed: the Pi needs no
# SVG renderer, and the text baked into each image does not depend on the
# fonts installed there. Re-run after changing an SVG.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${HERE}/../../control-daemon/data/img"
OUT="${HERE}/../src/display_client/icons"
# Tall enough to downscale cleanly onto either panel; the strip is never
# more than ~90 px high.
HEIGHT=240

command -v rsvg-convert >/dev/null || { echo "rsvg-convert not found (apt install librsvg2-bin)" >&2; exit 1; }
mkdir -p "${OUT}"
for svg in "${SRC}"/*.svg; do
  name="$(basename "${svg}" .svg)"
  rsvg-convert --keep-aspect-ratio --height "${HEIGHT}" "${svg}" -o "${OUT}/${name}.png"
  echo "${name}.png"
done
