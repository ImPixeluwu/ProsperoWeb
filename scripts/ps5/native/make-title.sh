#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Package ProsperoBrowser as a native PS5 title PRSB00001: a home-screen tile
# with icon and backgrounds, a signed development FSELF eboot.bin, the
# clean-room libc.prx runtime companion and sce_sys/param.json, plus the
# ProsperoStore catalog artifact (build-dist/PRSB00001.zip, PRSB00001/ at the
# top). See docs/native-title.md for the design, the launch chain and the
# frontend code changes still needed.
#
#   scripts/ps5/native/make-title.sh [path/to/native-app.elf]
#
# The eboot.bin:
#   - If an ELF is given (or dist/prosperobrowser/prosperobrowser.elf exists)
#     AND it was linked as a NATIVE app (third_party ps5-pie.ld + app_crt,
#     entry _start(process_parameters, teardown)), it is converted and signed
#     into the title's eboot.bin.
#   - A plain payload-SDK ELF (the browser as built today) is NOT a native
#     app: the converter rejects it ("no room for PS5 process parameters").
#     The script then builds a small native loader stub from
#     scripts/ps5/native/stub/ so the tile is complete, installable and
#     validatable now, and prints a loud warning. Replace it with the real
#     eboot.bin once the frontend is relinked natively (docs/native-title.md).
#
# Host tools are built from vendored source (third_party/ps5-native-tool,
# third_party/bc7enc_rdo); no Sony SDK or ISPC toolchain is used. Light builds
# land in build-native/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
TITLE_ID="PRSB00001"
INPUT_ELF="${1:-${ROOT}/dist/prosperobrowser/prosperobrowser.elf}"

NT="${ROOT}/third_party/ps5-native-tool"
BC="${ROOT}/third_party/bc7enc_rdo"
WORK="${ROOT}/build-native"
HOST="${WORK}/host"
OUTDIR="${ROOT}/build-dist/native"
APP="${OUTDIR}/${TITLE_ID}"
BG="${ROOT}/build-dist/assets/background-3840x2160.png"
DIST="${ROOT}/dist/prosperobrowser"

# Loader/container constants, verified on firmware 6.02/12.70 by the vendored
# boilerplate; kept separate from the public app version in param.json.
MODULE_SDK=0x02000009
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F

log() { printf '[native] %s\n' "$*"; }
die() { printf '[native] error: %s\n' "$*" >&2; exit 1; }

CXX="${CXX:-$(command -v clang++-18 || command -v clang++)}"
GXX="${GXX:-$(command -v g++)}"
CLANG18="${PS5_CLANG:-$(command -v clang-18 || command -v clang)}"
[[ -n "${CXX}" ]] || die "clang++ is required for the host tools"
[[ -n "${GXX}" ]] || die "g++ (with OpenMP) is required to build bc7enc"
[[ -d "${SDK}" ]] || die "PS5_PAYLOAD_SDK not found at ${SDK}"

mkdir -p "${HOST}"

# --- 1. Build the host tools from vendored source. --------------------------
build_native_tool() {
    [[ -x "${HOST}/ps5-native-tool" && "${HOST}/ps5-native-tool" -nt "${NT}/tooling/native/native_app_builder.cpp" ]] && return
    log "building ps5-native-tool"
    local n="${NT}/tooling/native"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/native_app_builder.cpp" -o "${HOST}/native_app_builder.o"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/self_container.cpp"     -o "${HOST}/self_container.o"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/elf_object.cpp"         -o "${HOST}/elf_object.o"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/sce_module_writer.cpp"  -o "${HOST}/sce_module_writer.o"
    "${CXX}" -std=c++20 -O2 -w "${HOST}"/{native_app_builder,self_container,elf_object,sce_module_writer}.o \
        -lz -o "${HOST}/ps5-native-tool"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/libc_builder.cpp" -o "${HOST}/libc_builder.o"
    "${CXX}" "${HOST}/libc_builder.o" -lz -o "${HOST}/libc-builder"
}

build_bc7enc() {
    [[ -x "${HOST}/bc7enc" ]] && return
    log "building bc7enc (CPU BC7, no ISPC)"
    ( cd "${BC}" && "${GXX}" -O2 -w -DSUPPORT_BC7E=0 -fopenmp -std=c++14 -include cstdint \
        test.cpp rdo_bc_encoder.cpp bc7enc.cpp bc7decomp.cpp bc7decomp_ref.cpp \
        rgbcx.cpp ert.cpp lodepng.cpp utils.cpp -pthread -o "${HOST}/bc7enc" )
}

build_native_tool
build_bc7enc

# --- 2. Clean-room libc.prx, checksum-gated against the vendored digest. -----
make_libc() {
    local manifest="${NT}/tooling/native/runtime/libc.prx.sha256"
    "${HOST}/libc-builder" "${NT}/tooling/native/runtime/api-surface.txt" \
        "${NT}/tooling/native/runtime/imports.txt" "${WORK}/libc.raw.elf" >/dev/null
    "${HOST}/ps5-native-tool" self --sign --in "${WORK}/libc.raw.elf" \
        --out "${WORK}/libc.prx" >/dev/null
    local want got
    want="$(awk '{print $1}' "${manifest}")"
    got="$(sha256sum "${WORK}/libc.prx" | cut -d' ' -f1)"
    [[ "${got}" == "${want}" ]] || die "libc.prx digest mismatch: ${got} != ${want}"
    log "libc.prx reproduced (${got})"
}
make_libc

# --- 3. Home-screen backgrounds: 3840x2160 BC7 DDS from the 4K PNG. ----------
make_dds() {
    [[ -f "${BG}" ]] || die "${BG} not found; run scripts/assets/render.sh first"
    "${HOST}/bc7enc" -C -u4 -g -f -o "${BG}" "${WORK}/pic0.dds" >/dev/null 2>&1 \
        || die "bc7enc failed"
    # bc7enc writes dwMipMapCount=0; the shell wants a single surface (=1).
    python3 -I - "${WORK}/pic0.dds" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
struct.pack_into("<I", d, 28, 1)
open(sys.argv[1], "wb").write(d)
PY
    cp -- "${WORK}/pic0.dds" "${WORK}/pic1.dds"
    log "pic0.dds / pic1.dds: 3840x2160 BC7_UNORM DX10 ($(stat -c %s "${WORK}/pic0.dds") bytes)"
}
make_dds

# --- 4. param.json from assets/app.json (single source of truth). -----------
make_param() {
    python3 -I - "${ROOT}/assets/app.json" "${WORK}/param.json" <<'PY'
import json, re, sys
app = json.load(open(sys.argv[1], encoding="utf-8"))
tid = app["titleId"]
assert re.fullmatch(r"[A-Z]{4}\d{5}", tid), f"bad titleId {tid!r}"
assert re.fullmatch(r"[A-Z]{2}\d{4}-" + tid + r"_00-[A-Z0-9]{16}", app["contentId"])
assert re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", app["contentVersion"])
param = {
    # Category 0 + badge 1 is the Games shelf, as ProsperoStore and websrv's
    # own loader title use it; this gives the title the game memory budget.
    "applicationCategoryType": 0,
    "applicationDrmType": "free",
    "attribute": 0,
    "conceptId": tid[4:],
    "contentBadgeType": 1,
    "contentId": app["contentId"],
    "contentVersion": app["contentVersion"],
    # Positive so the sandbox /download0 scratch mount exists; the writable
    # browser profile itself lives at /data/prosperobrowser (ShadowMountPlus
    # mounts /data writable via nullfs for a homebrew title).
    "downloadDataSize": 256,
    "localizedParameters": {
        "defaultLanguage": app["language"],
        app["language"]: {"titleName": app["name"]},
    },
    "masterVersion": app["masterVersion"],
    "requiredSystemSoftwareVersion": "0x0000000000000000",
    "sdkVersion": "0x0000000000000000",
    "titleId": tid,
}
with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
    json.dump(param, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY
    log "param.json generated from assets/app.json"
}
make_param

# --- 5. eboot.bin: convert the native-app ELF, else build the loader stub. --
STUB_USED=0
make_eboot() {
    local pie="$1" out="$2"
    if "${HOST}/ps5-native-tool" link --in "${pie}" --out "${WORK}/eboot.elf" \
        --stub-dir "${SDK}/target/lib" --module-sdk "${MODULE_SDK}" \
        --companion-sdk "${COMPANION_SDK}" --heap-size 0xffffffffffffffff \
        --file-name eboot.elf >/dev/null 2>&1; then
        "${HOST}/ps5-native-tool" self --sign --in "${WORK}/eboot.elf" \
            --out "${out}" --magic "${FSELF_MAGIC}" >/dev/null
        return 0
    fi
    return 1
}

build_stub() {
    local stub="${ROOT}/scripts/ps5/native/stub/stub_main.c"
    local n="${NT}/tooling/native"
    log "building native loader stub from scripts/ps5/native/stub/"
    sh "${NT}/tooling/prospero-clang18" -O2 -c "${n}/app_crt.cpp"         -o "${WORK}/app_crt.o"
    sh "${NT}/tooling/prospero-clang18" -O2 -c "${n}/app_cpp_runtime.cpp" -o "${WORK}/app_cpp_runtime.o"
    sh "${NT}/tooling/prospero-clang18" -O2 -c "${stub}"                  -o "${WORK}/stub_main.o"
    "${SDK}/bin/prospero-lld" -T "${n}/ps5-pie.ld" --eh-frame-hdr \
        --version-script "${n}/app-symbols.map" -e _start -o "${WORK}/stub-pie.elf" \
        "${WORK}"/{app_crt,app_cpp_runtime,stub_main}.o \
        --as-needed "${SDK}"/target/lib/*.so
    make_eboot "${WORK}/stub-pie.elf" "${APP}/eboot.bin" \
        || die "could not convert the loader stub"
}

PS5_PAYLOAD_SDK="${SDK}"; export PS5_PAYLOAD_SDK
rm -rf -- "${APP}"
mkdir -p "${APP}/sce_sys" "${APP}/sce_module"

if [[ -f "${INPUT_ELF}" ]] && make_eboot "${INPUT_ELF}" "${APP}/eboot.bin"; then
    log "eboot.bin: converted and signed ${INPUT_ELF}"
else
    if [[ -f "${INPUT_ELF}" ]]; then
        log "WARNING: ${INPUT_ELF} is not a native-app ELF (payload-SDK layout)."
        log "WARNING: using a loader stub as eboot.bin so the tile is complete."
        log "WARNING: relink the browser natively for the real eboot (docs/native-title.md)."
    else
        log "no input ELF; building the loader stub as eboot.bin"
    fi
    STUB_USED=1
    build_stub
fi

# --- 6. Assemble the title folder. ------------------------------------------
cp -- "${WORK}/libc.prx"  "${APP}/sce_module/libc.prx"
cp -- "${WORK}/param.json" "${APP}/sce_sys/param.json"
cp -- "${ROOT}/assets/sce_sys/icon0.png" "${APP}/sce_sys/icon0.png"
cp -- "${WORK}/pic0.dds" "${APP}/sce_sys/pic0.dds"
cp -- "${WORK}/pic1.dds" "${APP}/sce_sys/pic1.dds"

# Read-only runtime data under the title (served as /app0/data/...). The
# writable profile stays at /data/prosperobrowser (ShadowMountPlus nullfs).
# NATIVE_SKIP_DATA=1 leaves it out (smaller zip for a format-only check).
if [[ "${NATIVE_SKIP_DATA:-0}" != 1 && -d "${DIST}" ]]; then
    mkdir -p "${APP}/data"
    cp -a "${DIST}/chrome" "${APP}/data/chrome"
    cp -a "${DIST}/fonts"  "${APP}/data/fonts"
    cp -a "${DIST}/LICENSES" "${APP}/data/LICENSES"
    [[ -f "${DIST}/cacert.pem" ]] && cp -- "${DIST}/cacert.pem" "${APP}/data/cacert.pem"
    [[ -f "${DIST}/config.json.sample" ]] && cp -- "${DIST}/config.json.sample" "${APP}/data/config.json.sample"
    log "staged read-only runtime data under ${TITLE_ID}/data/"
else
    log "runtime data not staged (NATIVE_SKIP_DATA=1 or dist/ absent)"
fi

# --- 7. Validate the presentation assets (self-contained check). ------------
python3 -I - "${APP}/sce_sys" <<'PY'
from pathlib import Path
import struct, sys
root = Path(sys.argv[1])
icon = (root / "icon0.png").read_bytes()
assert icon[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", icon[16:24]) == (512, 512), \
    "icon0.png must be a 512x512 PNG"
for name in ("pic0.dds", "pic1.dds"):
    d = (root / name).read_bytes()
    hsz = struct.unpack_from("<I", d, 4)[0]
    h, w = struct.unpack_from("<II", d, 12)
    mips = struct.unpack_from("<I", d, 28)[0]
    fmt, dim = struct.unpack_from("<II", d, 128)
    arr = struct.unpack_from("<I", d, 140)[0]
    exp = 148 + w // 4 * (h // 4) * 16
    got = (hsz, w, h, mips, d[84:88], fmt, dim, arr, len(d))
    assert got == (124, 3840, 2160, 1, b"DX10", 98, 3, 1, exp), f"{name} bad DDS profile: {got}"
print("presentation assets validated:", root)
PY

# --- 8. ProsperoStore catalog artifact: PRSB00001.zip, PRSB00001/ at top. ---
ZIP="${ROOT}/build-dist/${TITLE_ID}.zip"
rm -f -- "${ZIP}"
# Built with python's zipfile (no external zip dependency): PRSB00001/ at the
# top of the archive, with eboot.bin kept executable (0755) so a console-side
# unzip preserves the bit the loader needs.
python3 -I - "${OUTDIR}" "${TITLE_ID}" "${ZIP}" <<'PY'
import os, stat, sys, zipfile
base, tid, out = sys.argv[1], sys.argv[2], sys.argv[3]
root = os.path.join(base, tid)
paths = []
for dirpath, _dirs, files in os.walk(root):
    for f in sorted(files):
        paths.append(os.path.join(dirpath, f))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for p in sorted(paths):
        arc = os.path.relpath(p, base)
        zi = zipfile.ZipInfo(arc)
        zi.compress_type = zipfile.ZIP_DEFLATED
        mode = 0o755 if os.path.basename(p) == "eboot.bin" else 0o644
        zi.external_attr = (stat.S_IFREG | mode) << 16
        with open(p, "rb") as fh:
            z.writestr(zi, fh.read())
PY

# --- Report. ----------------------------------------------------------------
log "title tree:"
( cd "${OUTDIR}" && find "${TITLE_ID}" -maxdepth 2 -type f -printf '%12s  %p\n' | sort -k2 )
log "eboot.bin inspect:"
"${HOST}/ps5-native-tool" self --inspect --file "${APP}/eboot.bin" | sed 's/^/[native]   /'
log "artifact: ${ZIP} ($(stat -c %s "${ZIP}") bytes, sha256 $(sha256sum "${ZIP}" | cut -c1-16)...)"
[[ "${STUB_USED}" == 1 ]] && log "NOTE: eboot.bin is the LOADER STUB, not the browser (see docs/native-title.md)."
log "done"
