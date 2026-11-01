#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch the ad-block filter lists pinned in addons.lock, convert them to the
# WebKit content-rule-list JSON, and stage them (with the SponsorBlock user
# script) for the PS5 package:
#
#   scripts/ps5/addons/stage-addons.sh <addons-dir> <licenses-dir> <install-dir>
#
#   <addons-dir>    receives adblock-ads.json, adblock-cookie.json,
#                   sponsorblock.js and sources.json
#   <licenses-dir>  receives the filter-list attributions
#   <install-dir>   where the package lives on the console (/data/prosperoweb);
#                   recorded in sources.json for reference
#
# The conversion uses the SAME code the on-device updater runs: the converter
# tool (tools/addons_convert) wraps prosperoweb::core::adblock::convert
# (src/core/adblock.cpp). If $ADDONS_CONVERT points at a prebuilt tool it is
# used; otherwise this script compiles it from those sources. So a bundled
# ruleset and one recompiled on the console are identical for identical input.
#
# Downloaded lists are untrusted data: cached under third_party/dl/addons/,
# verified against addons.lock's sha256, and only ever parsed (never executed).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
LOCK="${HERE}/addons.lock"
DL="${ROOT}/third_party/dl/addons"

ADDONS="${1:?addons dir}"
LICENSES="${2:?licenses dir}"
INSTALL_DIR="${3:?install dir}"

# WebKit's content-extension parser caps a compiled list at 150000 rules.
MAX_RULES="${ADDONS_MAX_RULES:-150000}"

log() { printf '[addons] %s\n' "$*"; }
die() { printf '[addons] error: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null || die "curl is required"
command -v sha256sum >/dev/null || die "sha256sum is required"

mkdir -p "${DL}" "${ADDONS}" "${LICENSES}"

# --- The converter tool: prebuilt or compiled from the shared sources. ------
CONVERT="${ADDONS_CONVERT:-}"
if [[ -z "${CONVERT}" || ! -x "${CONVERT}" ]]; then
  CONVERT="${DL}/addons_convert"
  CXX="${CXX:-c++}"
  log "building the converter with ${CXX}"
  "${CXX}" -std=c++20 -O2 \
    -I "${ROOT}/src/core/include" -I "${ROOT}/third_party" \
    "${ROOT}/tools/addons_convert/main.cpp" "${ROOT}/src/core/adblock.cpp" \
    -o "${CONVERT}" || die "could not build the converter"
fi
log "converter: ${CONVERT}"

# fetch <name> <sha256> <url>: download into the cache unless a verified copy
# is already there. These are live lists, so the pin drifts: ADDONS_ALLOW_
# UNPINNED=1 takes whatever upstream serves now (and prints the new sha256 to
# fold back into addons.lock) instead of failing on a mismatch.
fetch() {
  local name="$1" sum="$2" url="$3" out="${DL}/$1"
  if [[ -f "${out}" ]] && echo "${sum}  ${out}" | sha256sum --check --quiet - 2>/dev/null; then
    return
  fi
  log "downloading ${url}"
  curl -fsSL --retry 3 -o "${out}.part" "${url}"
  if ! echo "${sum}  ${out}.part" | sha256sum --check --quiet - 2>/dev/null; then
    if [[ "${ADDONS_ALLOW_UNPINNED:-0}" == "1" ]]; then
      log "WARNING ${name}: sha256 drifted to $(sha256sum "${out}.part" | cut -d' ' -f1) (unpinned)"
    else
      die "${name}: sha256 mismatch (upstream changed; set ADDONS_ALLOW_UNPINNED=1 or refresh addons.lock)"
    fi
  fi
  mv -f -- "${out}.part" "${out}"
}

ADS_INPUTS=()
COOKIE_INPUTS=()
ADS_SOURCES=()
COOKIE_SOURCES=()

while read -r kind name sum url; do
  [[ -z "${kind}" || "${kind}" == \#* ]] && continue
  [[ -n "${url}" ]] || die "malformed addons.lock line for ${name}"
  fetch "${name}" "${sum}" "${url}"
  case "${kind}" in
    ads) ADS_INPUTS+=("${DL}/${name}"); ADS_SOURCES+=("${url}") ;;
    cookie) COOKIE_INPUTS+=("${DL}/${name}"); COOKIE_SOURCES+=("${url}") ;;
    *) die "unknown kind ${kind} for ${name}" ;;
  esac
done < "${LOCK}"

[[ ${#ADS_INPUTS[@]} -gt 0 ]] || die "no ads lists in addons.lock"

# ENABLE(CONTENT_EXTENSIONS) is off in our WebKit build, so the console cannot
# use native content rule lists. Produce the userscript filter set (host
# blocklist + cosmetic CSS + domain rules) that adblock.js enforces instead.
log "building ads filter set (EasyList/EasyPrivacy/uBlock) -> adblock-ads.filters.json"
"${CONVERT}" --filter-set "${ADDONS}/adblock-ads.filters.json" "${ADS_INPUTS[@]}"
if [[ ${#COOKIE_INPUTS[@]} -gt 0 ]]; then
  log "building cookie filter set -> adblock-cookie.filters.json"
  "${CONVERT}" --filter-set "${ADDONS}/adblock-cookie.filters.json" "${COOKIE_INPUTS[@]}"
fi

# --- User scripts. ----------------------------------------------------------
cp -- "${ROOT}/src/addons/adblock.js" "${ADDONS}/adblock.js"
cp -- "${ROOT}/src/addons/sponsorblock.js" "${ADDONS}/sponsorblock.js"

# --- sources.json: what the on-device updater refetches. --------------------
# A JSON manifest the runtime reads to know the source URLs (so online updates
# do not need a new browser release to change a URL).
join_json() {
  local first=1 u
  for u in "$@"; do
    [[ ${first} -eq 1 ]] && first=0 || printf ', '
    printf '"%s"' "${u}"
  done
}
{
  printf '{\n'
  printf '  "install_dir": "%s",\n' "${INSTALL_DIR}"
  printf '  "ads": { "output": "adblock-ads.filters.json", "sources": [%s] },\n' "$(join_json "${ADS_SOURCES[@]}")"
  printf '  "cookie": { "output": "adblock-cookie.filters.json", "sources": [%s] }\n' "$(join_json "${COOKIE_SOURCES[@]}")"
  printf '}\n'
} > "${ADDONS}/sources.json"

# --- Licenses / attribution. ------------------------------------------------
cat > "${LICENSES}/filter-lists.txt" <<'EOF'
Ad-block filter lists bundled with ProsperoWeb
==============================================

ProsperoWeb's ad blocker is NOT uBlock Origin. Because our WebKit build has
content extensions disabled, it enforces a SUBSET of these public lists with
an injected userscript: whole-host network blocking and static element-hiding
CSS (generic and domain-scoped). uBlock's scriptlets and procedural cosmetic
filters are skipped. Coverage is best-effort and weaker than uBlock Origin.

  EasyList            https://easylist.to/   (GPLv3 and CC BY-SA 3.0)
  EasyPrivacy         https://easylist.to/   (GPLv3 and CC BY-SA 3.0)
  EasyList Cookie List (Fanboy's Cookiemonster, "Block cookie banners")
                      https://easylist.to/   (GPLv3 and CC BY-SA 3.0)
  uBlock Origin filter lists (uAssets: filters, filters-general, filters-
                      mobile, filters-2020..2026, privacy, badware, unbreak,
                      quick-fixes)
                      https://github.com/uBlockOrigin/uAssets   (GPLv3)
  Peter Lowe's Ad and tracking server list
                      https://pgl.yoyo.org/adservers/   (CC BY-NC-SA 4.0)

The EasyList authors dual-license their lists under the GNU General Public
License version 3 (GPL-3.0-or-later.txt, shipped alongside) and the Creative
Commons Attribution-ShareAlike 3.0 Unported License
(https://creativecommons.org/licenses/by-sa/3.0/). uBlock Origin's uAssets
lists are licensed under the GPL version 3. The lists are bundled unmodified
as downloaded; ProsperoWeb extracts the host/cosmetic subset at build time and
may refetch and rebuild it on the console.

SponsorBlock
------------
addons/sponsorblock.js is ProsperoWeb's own user script (GPL-3.0-or-later),
NOT the SponsorBlock browser extension. It queries the public SponsorBlock
API at https://sponsor.ajay.app, whose crowd-sourced segment data is licensed
CC BY-NC-SA 4.0 (https://sponsor.ajay.app/). SponsorBlock is a trademark of
its authors; this is an independent client.
EOF

log "staged $(find "${ADDONS}" -type f | wc -l) files, $(du -sh "${ADDONS}" | cut -f1)"
