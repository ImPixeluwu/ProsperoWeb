#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch the fonts pinned in fonts.lock and stage them for the PS5 package:
#
#   scripts/ps5/fonts/stage-fonts.sh <fonts-dir> <licenses-dir> <install-dir>
#
#   <fonts-dir>     receives the font files and fonts.conf
#   <licenses-dir>  receives one license text per font family
#   <install-dir>   where the package lives on the console (/data/prosperoweb);
#                   fonts.conf points <dir> and <cachedir> below it
#
# Downloads are cached in third_party/dl/fonts/ and checked against the
# sha256 in fonts.lock on every run. Archives are treated as data: each is
# unpacked into its own empty directory under build-deps/fonts/ and only
# named files are copied out.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
LOCK="${HERE}/fonts.lock"
DL="${ROOT}/third_party/dl/fonts"
UNPACK="${ROOT}/build-deps/fonts"

FONTS="${1:?fonts dir}"
LICENSES="${2:?licenses dir}"
INSTALL_DIR="${3:?install dir}"

log() { printf '[fonts] %s\n' "$*"; }
die() { printf '[fonts] error: %s\n' "$*" >&2; exit 1; }

mkdir -p "${DL}" "${UNPACK}" "${FONTS}" "${LICENSES}"

# fetch <name> <sha256> <url>: download into the cache unless a verified
# copy is already there.
fetch() {
    local name="$1" sum="$2" url="$3" out="${DL}/$1"
    if [[ -f "${out}" ]] && echo "${sum}  ${out}" | sha256sum --check --quiet - 2>/dev/null; then
        return
    fi
    log "downloading ${url}"
    curl -fsSL --retry 3 -o "${out}.part" "${url}"
    echo "${sum}  ${out}.part" | sha256sum --check --quiet - || die "${name}: sha256 mismatch"
    mv -f -- "${out}.part" "${out}"
}

# unpack <archive>: extract into a fresh build-deps/fonts/<archive>/.
unpack() {
    local dir="${UNPACK}/$1"
    rm -rf -- "${dir}"
    mkdir -p "${dir}"
    tar -xf "${DL}/$1" -C "${dir}" --no-same-owner --no-same-permissions
    printf '%s\n' "${dir}"
}

license_name() {
    case "$1" in
        notofonts.github.io-LICENSE) echo "Noto-fonts-OFL-1.1.txt" ;;
        noto-emoji-LICENSE) echo "Noto-Color-Emoji-OFL-1.1.txt" ;;
        noto-cjk-LICENSE) echo "Noto-Sans-CJK-OFL-1.1.txt" ;;
        *) die "no license name for $1" ;;
    esac
}

while read -r kind name sum url; do
    [[ -z "${kind}" || "${kind}" == \#* ]] && continue
    [[ -n "${url}" ]] || die "malformed fonts.lock line for ${name}"
    fetch "${name}" "${sum}" "${url}"
    case "${kind}" in
        font) cp -- "${DL}/${name}" "${FONTS}/${name}" ;;
        license) cp -- "${DL}/${name}" "${LICENSES}/$(license_name "${name}")" ;;
        archive)
            dir="$(unpack "${name}")"
            case "${name}" in
                dejavu-fonts-ttf-*)
                    src="${dir}/${name%.tar.bz2}"
                    [[ -d "${src}/ttf" ]] || die "unexpected DejaVu tarball layout"
                    # Not DejaVuMathTeXGyre.ttf: it has the widest coverage and
                    # wins family-less fallback matches, rendering body text in
                    # the math face.
                    find "${src}/ttf" -name 'DejaVu*.ttf' ! -name 'DejaVuMathTeXGyre.ttf' -exec cp -- {} "${FONTS}/" \;
                    cp -- "${src}/LICENSE" "${FONTS}/LICENSE-DejaVu"
                    cp -- "${src}/LICENSE" "${LICENSES}/DejaVu-fonts.txt"
                    ;;
                liberation-fonts-ttf-*)
                    src="${dir}/${name%.tar.gz}"
                    for f in Sans Serif Mono; do
                        for s in Regular Bold Italic BoldItalic; do
                            cp -- "${src}/Liberation${f}-${s}.ttf" "${FONTS}/"
                        done
                    done
                    cp -- "${src}/LICENSE" "${LICENSES}/Liberation-fonts-OFL-1.1.txt"
                    ;;
                *) die "no install rule for archive ${name}" ;;
            esac
            ;;
        *) die "unknown kind ${kind} for ${name}" ;;
    esac
done < "${LOCK}"

# --- fonts.conf --------------------------------------------------------------
# fontconfig is compiled with /user/homebrew/etc/fonts/fonts.conf, which does
# not exist on the console; the frontend points FONTCONFIG_FILE here. No
# conf.d is loaded, so every family mapping lives in this file.
#
# Skia's fontconfig font manager drops weakly bound families after the last
# strong one and rejects a match whose family is not in what is left, so the
# aliases bind "same" (as strong as the requested name) and the catch-all
# binds "strong". Each <alias> tests a single family: aliases listing several
# families did not take effect for all of them.
SANS=("Noto Sans" "Liberation Sans" "DejaVu Sans" "Noto Sans CJK SC" "Noto Color Emoji")
SERIF=("Noto Serif" "Liberation Serif" "DejaVu Serif" "Noto Sans CJK SC" "Noto Color Emoji")
MONO=("Liberation Mono" "Noto Sans Mono" "DejaVu Sans Mono" "Noto Sans Mono CJK SC" "Noto Color Emoji")
EMOJI=("Noto Color Emoji")

# alias <family> <preferred>...: one alias block for one requested family.
alias_block() {
    local family="$1" f
    shift
    printf '  <alias binding="same"><family>%s</family><prefer>' "${family}"
    for f in "$@"; do printf '<family>%s</family>' "${f}"; done
    printf '</prefer></alias>\n'
}

# cjk_lang <lang> <family>: append the CJK face for one language.
cjk_lang() {
    printf '  <match target="pattern"><test name="lang" compare="eq"><string>%s</string></test>' "$1"
    printf '<edit name="family" mode="append_last" binding="strong"><string>%s</string></edit></match>\n' "$2"
}

{
    cat <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<!-- Generated by scripts/ps5/fonts/stage-fonts.sh. -->
<fontconfig>
  <dir>${INSTALL_DIR}/fonts</dir>
  <cachedir>${INSTALL_DIR}/cache/fontconfig</cachedir>
  <config><rescan><int>0</int></rescan></config>

  <!-- CSS generic families. -->
EOF
    for f in sans-serif system-ui ui-sans-serif cursive fantasy; do alias_block "${f}" "${SANS[@]}"; done
    for f in serif ui-serif; do alias_block "${f}" "${SERIF[@]}"; done
    for f in monospace ui-monospace; do alias_block "${f}" "${MONO[@]}"; done
    for f in emoji; do alias_block "${f}" "${EMOJI[@]}"; done

    echo
    echo '  <!-- Metric-compatible substitutes first, then the generic list. -->'
    for f in Arial Helvetica; do alias_block "${f}" "Liberation Sans" "${SANS[@]}"; done
    for f in Times "Times New Roman"; do alias_block "${f}" "Liberation Serif" "${SERIF[@]}"; done
    for f in Courier "Courier New"; do alias_block "${f}" "${MONO[@]}"; done

    echo
    echo '  <!-- Common platform and web families that are not installed. -->'
    for f in -apple-system BlinkMacSystemFont "Segoe UI" Roboto "Helvetica Neue" Verdana Tahoma \
             "Open Sans" Inter Ubuntu "Lucida Grande" "Trebuchet MS" "Noto Sans UI"; do
        alias_block "${f}" "${SANS[@]}"
    done
    for f in Georgia Cambria "Palatino Linotype" "Book Antiqua"; do alias_block "${f}" "${SERIF[@]}"; done
    for f in Consolas Menlo Monaco "Lucida Console" "SF Mono" "Roboto Mono"; do alias_block "${f}" "${MONO[@]}"; done
    for f in "Apple Color Emoji" "Segoe UI Emoji" "Segoe UI Symbol" "Android Emoji" "Twemoji Mozilla" "EmojiOne Color"; do
        alias_block "${f}" "${EMOJI[@]}"
    done

    cat <<'EOF'

  <!-- Catch-all, after every alias: any other family, and a request with
       no family, still resolves to a font. Without it an unknown family
       matches nothing and WebKit ends up in its last-resort path. -->
  <match target="pattern">
    <edit name="family" mode="append_last" binding="strong"><string>Noto Sans</string></edit>
  </match>

  <!-- Han fallback: every face of the CJK collection claims every CJK
       language, so without these the JP face wins for Chinese and Korean.
       WebKit's per-character fallback asks with the page language and no
       family, so this decides which glyph variant a page gets. -->
EOF
    cjk_lang ja "Noto Sans CJK JP"
    cjk_lang ko "Noto Sans CJK KR"
    cjk_lang zh-tw "Noto Sans CJK TC"
    cjk_lang zh-hk "Noto Sans CJK HK"
    cjk_lang zh-mo "Noto Sans CJK HK"
    cjk_lang zh-cn "Noto Sans CJK SC"
    cjk_lang zh-sg "Noto Sans CJK SC"
    cjk_lang zh "Noto Sans CJK SC"
    cat <<'EOF'
</fontconfig>
EOF
} > "${FONTS}/fonts.conf"

log "staged $(find "${FONTS}" -type f \( -name '*.ttf' -o -name '*.ttc' \) | wc -l) font files, $(du -sh "${FONTS}" | cut -f1)"
