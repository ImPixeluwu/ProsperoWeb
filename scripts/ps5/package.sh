#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Assemble the PS5 distribution from a linked ps5browser.elf:
#
#   dist/ps5browser/            -> copy to /data/ps5browser/ on the console
#     ps5browser.elf              debug info stripped (symbols kept)
#     chrome/                     HTML chrome (src/chrome-ui)
#     cacert.pem                  curl's CA bundle (MPL-2.0)
#     fonts/                      DejaVu TTFs + fonts.conf (FONTCONFIG_FILE)
#     config.json.sample          copy to config.json to override defaults
#     LICENSES/                   GPL-3.0 (ps5browser), MPL-2.0 notice (cacert)
#     README-install.txt
#   dist/homebrew/ps5browser/   -> copy to /data/homebrew/ps5browser/ (websrv tile)
#     homebrew.js                 launches /data/ps5browser/ps5browser.elf
#     sce_sys/icon0.png           512x512 tile icon
#
#   scripts/ps5/package.sh [path/to/ps5browser.elf]
#
# Downloads are cached and verified in build-dist/dl/ (cacert.pem against
# curl.se's published .sha256, DejaVu against the sha256 pinned below).
# No Noto CJK: about 100 MB per weight; CJK text renders as missing glyphs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ELF="${1:-${ROOT}/build-ps5/src/frontend/ps5/ps5browser.elf}"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
WORK="${ROOT}/build-dist"
DL="${WORK}/dl"
DIST="${ROOT}/dist"
OUT="${DIST}/ps5browser"
TILE="${DIST}/homebrew/ps5browser"
INSTALL_DIR="/data/ps5browser"

CACERT_URL="https://curl.se/ca/cacert.pem"
DEJAVU_VERSION="2.37"
DEJAVU_URL="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_${DEJAVU_VERSION//./_}/dejavu-fonts-ttf-${DEJAVU_VERSION}.tar.bz2"
DEJAVU_SHA256="fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7"

log() { printf '[package] %s\n' "$*"; }
die() { printf '[package] error: %s\n' "$*" >&2; exit 1; }

[[ -f "${ELF}" ]] || die "${ELF} not found; build the ps5browser target first"
for t in curl sha256sum tar convert; do
    command -v "${t}" >/dev/null || die "${t} is required"
done

# download <url> <file>: fetch into the cache unless already there.
download() {
    local url="$1" out="${DL}/$2"
    if [[ ! -f "${out}" ]]; then
        log "downloading ${url}"
        curl -fsSL --retry 3 -o "${out}.part" "${url}"
        mv -f -- "${out}.part" "${out}"
    fi
}

mkdir -p "${DL}"

# --- CA bundle: verified against the checksum curl.se publishes. -----------
# Always refetched: the bundle changes, and a stale cached pair still verifies.
rm -f -- "${DL}/cacert.pem" "${DL}/cacert.pem.sha256"
download "${CACERT_URL}" cacert.pem
download "${CACERT_URL}.sha256" cacert.pem.sha256
(cd "${DL}" && sha256sum --check --quiet cacert.pem.sha256) || die "cacert.pem checksum mismatch"

# --- DejaVu fonts: pinned tarball, extracted into its own directory. -------
download "${DEJAVU_URL}" "dejavu-fonts-ttf-${DEJAVU_VERSION}.tar.bz2"
echo "${DEJAVU_SHA256}  ${DL}/dejavu-fonts-ttf-${DEJAVU_VERSION}.tar.bz2" | sha256sum --check --quiet - \
    || die "DejaVu tarball checksum mismatch"
DEJAVU_DIR="${WORK}/dejavu"
rm -rf -- "${DEJAVU_DIR}"
mkdir -p "${DEJAVU_DIR}"
tar -xjf "${DL}/dejavu-fonts-ttf-${DEJAVU_VERSION}.tar.bz2" -C "${DEJAVU_DIR}" --no-same-owner --no-same-permissions
DEJAVU_SRC="${DEJAVU_DIR}/dejavu-fonts-ttf-${DEJAVU_VERSION}"
[[ -d "${DEJAVU_SRC}/ttf" ]] || die "unexpected DejaVu tarball layout"

# --- Assemble. dist/ is generated output, rebuilt from scratch. -------------
rm -rf -- "${OUT}" "${TILE}"
mkdir -p "${OUT}/chrome" "${OUT}/fonts" "${OUT}/LICENSES" "${TILE}/sce_sys"

# elfldr and the spawner only need the program headers, the section headers
# and the SHT_RELA sections, so debug info can go. The symbol table stays
# for crash backtraces.
"${SDK}/bin/llvm-strip" --strip-debug -o "${OUT}/ps5browser.elf" "${ELF}"

cp -- "${ROOT}"/src/chrome-ui/*.html "${ROOT}"/src/chrome-ui/*.css "${ROOT}"/src/chrome-ui/*.js "${OUT}/chrome/"
cp -- "${DL}/cacert.pem" "${OUT}/cacert.pem"
# Not DejaVuMathTeXGyre.ttf: it has the widest coverage and wins
# family-less fallback matches, rendering body text in the math face.
find "${DEJAVU_SRC}/ttf" -name '*.ttf' ! -name 'DejaVuMathTeXGyre.ttf' -exec cp -- {} "${OUT}/fonts/" \;
cp -- "${DEJAVU_SRC}/LICENSE" "${OUT}/fonts/LICENSE-DejaVu"
cp -- "${ROOT}/LICENSE" "${OUT}/LICENSES/GPL-3.0-or-later.txt"
cp -- "${DEJAVU_SRC}/LICENSE" "${OUT}/LICENSES/DejaVu-fonts.txt"

cat > "${OUT}/LICENSES/cacert-MPL-2.0.txt" <<'EOF'
cacert.pem is the CA certificate bundle extracted from Mozilla's NSS by the
curl project (https://curl.se/docs/caextract.html). It is distributed under
the Mozilla Public License, version 2.0, unmodified: cacert.pem is itself the
Source Code Form. A copy of the license is available at
https://mozilla.org/MPL/2.0/.
EOF

cat > "${OUT}/LICENSES/README.txt" <<'EOF'
ps5browser.elf is free software under the GNU General Public License,
version 3 or later (GPL-3.0-or-later.txt). It statically links WebKit
(LGPL-2.1 / BSD), Skia (BSD-3-Clause), ICU (Unicode-3.0), FreeType (FTL),
HarfBuzz (MIT), libwpe (BSD-2-Clause), curl (curl), OpenSSL (Apache-2.0),
SQLite (public domain), libxml2 (MIT), fontconfig (MIT), SDL2 (Zlib) and
FFmpeg with x264, which makes the combined binary GPL. The corresponding
source is the ps5-browser repository at the commit this package was built
from, plus the upstream sources listed in its scripts/ps5/build-deps.sh and
scripts/ps5/fetch-webkit.sh.

Fonts: DejaVu (DejaVu-fonts.txt). CA bundle: MPL-2.0 (cacert-MPL-2.0.txt).
EOF

# fontconfig is compiled with /user/homebrew/etc/fonts/fonts.conf, which does
# not exist on the console; the frontend points FONTCONFIG_FILE here. No
# conf.d is loaded, so the generic families are mapped explicitly.
cat > "${OUT}/fonts/fonts.conf" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>${INSTALL_DIR}/fonts</dir>
  <cachedir>${INSTALL_DIR}/cache/fontconfig</cachedir>
  <config><rescan><int>0</int></rescan></config>

  <!-- Common web fonts that are not installed (before the generic
       families, so the appended generic family is expanded below). -->
EOF
for f in Arial Helvetica Verdana Tahoma "Segoe UI" Roboto -apple-system BlinkMacSystemFont; do
    printf '  <alias><family>%s</family><default><family>sans-serif</family></default></alias>\n' "${f}"
done >> "${OUT}/fonts/fonts.conf"
for f in Times "Times New Roman" Georgia; do
    printf '  <alias><family>%s</family><default><family>serif</family></default></alias>\n' "${f}"
done >> "${OUT}/fonts/fonts.conf"
for f in Courier "Courier New" Consolas Menlo Monaco; do
    printf '  <alias><family>%s</family><default><family>monospace</family></default></alias>\n' "${f}"
done >> "${OUT}/fonts/fonts.conf"
cat >> "${OUT}/fonts/fonts.conf" <<'EOF'
  <alias binding="same"><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias binding="same"><family>serif</family><prefer><family>DejaVu Serif</family></prefer></alias>
  <alias binding="same"><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
  <alias binding="same"><family>system-ui</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias binding="same"><family>cursive</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias binding="same"><family>fantasy</family><prefer><family>DejaVu Sans</family></prefer></alias>
</fontconfig>
EOF

# Defaults from src/frontend/ps5/ps5_config.h; config.json overrides them.
cat > "${OUT}/config.json.sample" <<EOF
{
  "home_url": "https://duckduckgo.com/",
  "language": "en",
  "chrome_dir": "${INSTALL_DIR}/chrome",
  "width": 1920,
  "height": 1080
}
EOF

# websrv (ps5-payload-dev/websrv) lists /data/homebrew/<name>/ entries that
# have a homebrew.js or an eboot.elf, with sce_sys/icon0.png as the tile.
# homebrew.js wins over eboot.elf, so the tile launches the installed ELF
# in place instead of carrying a second 100+ MB copy.
cat > "${TILE}/homebrew.js" <<EOF
// SPDX-License-Identifier: GPL-3.0-or-later
// websrv launcher entry for ps5browser (installed in ${INSTALL_DIR}).
async function main() {
    return {
        mainText: "ps5browser",
        secondaryText: "WebKit web browser",
        onclick: async () => {
            return {
                path: "${INSTALL_DIR}/ps5browser.elf",
                cwd: "${INSTALL_DIR}",
                args: []
            };
        }
    };
}
EOF

convert -size 512x512 gradient:'#1b3a8a'-'#0a1430' \
    -fill white -font "${DEJAVU_SRC}/ttf/DejaVuSans-Bold.ttf" -pointsize 300 -gravity center -annotate +0-30 'W' \
    -fill '#c8d4ff' -font "${DEJAVU_SRC}/ttf/DejaVuSans.ttf" -pointsize 64 -gravity south -annotate +0+40 'ps5browser' \
    -strip "PNG24:${TILE}/sce_sys/icon0.png" 2>/dev/null \
    || convert -size 512x512 gradient:'#1b3a8a'-'#0a1430' -strip "PNG24:${TILE}/sce_sys/icon0.png"

cat > "${OUT}/README-install.txt" <<EOF
ps5browser - installation
=========================

Requirements: a jailbroken PS5 running ps5-payload-dev/elfldr (port 9021),
and for the launcher tile ps5-payload-dev/websrv (port 8080); ftpsrv
(port 2121) to copy the files.

1. Copy the files (FTP, ftpsrv on port 2121):

     ps5browser/          -> ${INSTALL_DIR}/
     homebrew/ps5browser/ -> /data/homebrew/ps5browser/   (optional, websrv tile)

   For example with lftp:

     lftp -p 2121 PS5_IP -e "mirror -R ps5browser ${INSTALL_DIR}; mirror -R homebrew/ps5browser /data/homebrew/ps5browser; quit"

   The browser must live in ${INSTALL_DIR}: it re-launches
   ${INSTALL_DIR}/ps5browser.elf for its WebProcess and NetworkProcess
   (override with PS5BROWSER_SELF_PATH), and reads its fonts, CA bundle
   and chrome from there.

2. Optional: copy config.json.sample to config.json and edit it.

3. Launch, one of:

   - websrv: open http://PS5_IP:8080/ (or the Homebrew Launcher app) and
     select the "ps5browser" tile.
   - elfldr, loading the installed file on the console:

       echo "file:${INSTALL_DIR}/ps5browser.elf" | nc -q0 PS5_IP 9021

   - elfldr, sending the ELF over the network (slower, same result):

       nc -q0 PS5_IP 9021 < ps5browser/ps5browser.elf

   elfldr reads a raw ELF, a file:/http(s): URI line, or an HTTP GET with
   ?uri=...; it does not accept an HTTP POST body.

Logs: ${INSTALL_DIR}/log.txt (UI process); stdout/stderr of all three
processes go to the console's debug output (klogsrv, port 3232).

Environment the browser sets for itself (unless already set):
  FONTCONFIG_FILE=${INSTALL_DIR}/fonts/fonts.conf
  FONTCONFIG_PATH=${INSTALL_DIR}/fonts
  PS5BROWSER_CA_BUNDLE=${INSTALL_DIR}/cacert.pem
  SYSTEM_LANGUAGE=en-US (from config.json "language")
Media: PS5BROWSER_MEDIA_AUDIO=null disables audio output.
EOF

# --- Report. ---------------------------------------------------------------
size_of() { stat -c %s "$1"; }
log "ps5browser.elf: $(size_of "${ELF}") bytes as linked, $(size_of "${OUT}/ps5browser.elf") bytes with --strip-debug"
"${SDK}/bin/llvm-strip" --strip-all -o "${WORK}/ps5browser.strip-all.elf" "${ELF}"
log "              ($(size_of "${WORK}/ps5browser.strip-all.elf") bytes with --strip-all, not shipped)"
log "dist tree:"
(cd "${DIST}" && find ps5browser homebrew -type f -printf '%10s  %p\n' | sort -k2)
du -sh "${OUT}" "${TILE}"
