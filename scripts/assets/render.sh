#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Render the home screen artwork from its SVG sources:
#
#   assets/icon0.svg       -> assets/sce_sys/icon0.png  512x512   launch icon
#   assets/background.svg  -> assets/sce_sys/pic0.png   1920x1080 selection background
#                          -> assets/sce_sys/pic1.png   1920x1080 launch background
#                          -> build-dist/assets/background-3840x2160.png
#                             (not committed; the source for the 3840x2160
#                             BC7 pic0.dds/pic1.dds of a native app folder)
#   docs/screenshots/store-icon-preview.png        icon at 512 and 128
#   docs/screenshots/store-background-preview.png  background at 1280x720
#
#   scripts/assets/render.sh
#
# The PNGs are committed so packaging needs no SVG renderer; rerun this
# after editing an SVG. Text is set in Noto Sans (SIL OFL 1.1) from the
# font cache that scripts/ps5/fonts/stage-fonts.sh fills and verifies;
# fontconfig is pointed at that font only, so the result does not depend
# on the fonts installed on the host.
#
# Needs rsvg-convert (apt-get install --no-install-recommends librsvg2-bin)
# and ImageMagick's convert.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="${ROOT}/assets"
OUT="${SRC}/sce_sys"
WORK="${ROOT}/build-dist/assets"
SHOTS="${ROOT}/docs/screenshots"
FONT_CACHE="${ROOT}/third_party/dl/fonts"

log() { printf '[assets] %s\n' "$*"; }
die() { printf '[assets] error: %s\n' "$*" >&2; exit 1; }

for t in rsvg-convert convert; do
    command -v "${t}" >/dev/null || die "${t} is required"
done

mkdir -p "${OUT}" "${WORK}/fonts" "${WORK}/fc-cache"

# Fonts: fetch and verify through stage-fonts.sh if the cache lacks them.
if [[ ! -f "${FONT_CACHE}/NotoSans-Bold.ttf" || ! -f "${FONT_CACHE}/NotoSans-Regular.ttf" ]]; then
    "${ROOT}/scripts/ps5/fonts/stage-fonts.sh" "${WORK}/staged-fonts" "${WORK}/staged-licenses" /tmp >/dev/null
fi
grep -E '^font NotoSans-(Bold|Regular)\.ttf ' "${ROOT}/scripts/ps5/fonts/fonts.lock" \
    | while read -r _ name sum _; do
        echo "${sum}  ${FONT_CACHE}/${name}" | sha256sum --check --quiet - || die "${name} checksum mismatch"
        cp -f -- "${FONT_CACHE}/${name}" "${WORK}/fonts/${name}"
    done
cat > "${WORK}/fonts.conf" <<CONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>${WORK}/fonts</dir>
  <cachedir>${WORK}/fc-cache</cachedir>
</fontconfig>
CONF
export FONTCONFIG_FILE="${WORK}/fonts.conf"

# render <svg> <width> <height> <png>: rasterise, then store as plain
# 8-bit RGB PNG without metadata (no timestamps, so reruns are byte-stable).
render() {
    rsvg-convert --width "$2" --height "$3" --format png "$1" \
        | convert png:- -alpha off -strip -define png:exclude-chunks=date,time "PNG24:$4"
}

log "icon0.png"
render "${SRC}/icon0.svg" 512 512 "${OUT}/icon0.png"
log "pic0.png, pic1.png"
render "${SRC}/background.svg" 1920 1080 "${OUT}/pic0.png"
cp -f -- "${OUT}/pic0.png" "${OUT}/pic1.png"
log "background-3840x2160.png (build-dist only)"
render "${SRC}/background.svg" 3840 2160 "${WORK}/background-3840x2160.png"

log "previews"
convert -size 800x560 xc:'#2f343d' \
    "${OUT}/icon0.png" -geometry +24+24 -composite \
    \( "${OUT}/icon0.png" -resize 128x128 \) -geometry +592+24 -composite \
    -strip -define png:exclude-chunks=date,time "PNG24:${SHOTS}/store-icon-preview.png"
convert "${OUT}/pic0.png" -resize 1280x720 -strip -define png:exclude-chunks=date,time \
    "PNG24:${SHOTS}/store-background-preview.png"

for f in "${OUT}"/icon0.png "${OUT}"/pic0.png "${OUT}"/pic1.png "${WORK}/background-3840x2160.png" \
         "${SHOTS}/store-icon-preview.png" "${SHOTS}/store-background-preview.png"; do
    printf '[assets]   %-60s %s\n' "${f#"${ROOT}"/}" "$(identify -format '%wx%h %[size]' "${f}")"
done
