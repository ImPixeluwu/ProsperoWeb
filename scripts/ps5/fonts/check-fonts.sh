#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Check the staged fonts.conf with the host's fontconfig:
#
#   scripts/ps5/fonts/check-fonts.sh [dist/ps5browser/fonts]
#
# fonts.conf names the console paths, so a copy pointing <dir> at the staged
# fonts and <cachedir> at a fresh, not yet existing directory is used. The
# check fails if a family resolves outside the bundled fonts or if fontconfig
# does not create the cache directory itself (it has to on the console).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
FONTS="$(cd "${1:-${ROOT}/dist/ps5browser/fonts}" && pwd)"
WORK="${ROOT}/build-dist/fontcheck"

command -v fc-match >/dev/null || { echo "fc-match is required" >&2; exit 1; }
rm -rf -- "${WORK}"
mkdir -p "${WORK}"
CACHE="${WORK}/cache/fontconfig"
sed -e "s#<dir>[^<]*</dir>#<dir>${FONTS}</dir>#" \
    -e "s#<cachedir>[^<]*</cachedir>#<cachedir>${CACHE}</cachedir>#" \
    "${FONTS}/fonts.conf" > "${WORK}/fonts.conf"
export FONTCONFIG_FILE="${WORK}/fonts.conf"

fail=0
check() {
    local got
    got="$(fc-match -f '%{family[0]} | %{style[0]} | %{file}\n' -- "$1")"
    printf '%-34s -> %s\n' "$1" "${got/${FONTS}\//}"
    [[ "${got}" == *"${FONTS}/"* ]] || fail=1
}

for f in sans-serif serif monospace system-ui cursive fantasy emoji \
         -apple-system BlinkMacSystemFont "Segoe UI" Roboto "Helvetica Neue" Helvetica Arial \
         Verdana Tahoma "Open Sans" Inter Ubuntu \
         Times "Times New Roman" Georgia \
         Courier "Courier New" Consolas Menlo Monaco \
         "Apple Color Emoji" "Segoe UI Emoji" \
         "Some Weird Font" "Arial:bold" "Times New Roman:italic" "sans-serif:bold:italic" ":"; do
    check "${f}"
done

for p in 'sans-serif:lang=ru' 'sans-serif:lang=ja' 'sans-serif:lang=zh-cn' 'sans-serif:lang=zh-tw' 'sans-serif:lang=ko' ':lang=ja' ':lang=und-zsye' ':charset=4e2d' ':charset=1f600'; do
    echo "fc-match -s '${p}' (first 6):"
    fc-match -s -f '    %{family[0]} | %{style[0]}\n' -- "${p}" | head -6
done

if [[ -d "${CACHE}" ]] && compgen -G "${CACHE}/*.cache-*" >/dev/null; then
    echo "cache dir created by fontconfig: $(ls "${CACHE}" | wc -l) file(s)"
else
    echo "fontconfig did not create ${CACHE}" >&2
    fail=1
fi
exit "${fail}"
