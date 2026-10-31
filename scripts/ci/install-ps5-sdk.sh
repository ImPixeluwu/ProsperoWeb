#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install the ps5-payload-sdk with the prebuilt pacbrew libraries (the
# README's "Build from scratch" step) for CI.
#
#   scripts/ci/install-ps5-sdk.sh [--dry-run]
#
# Environment:
#   PS5_SDK_VERSION  pacbrew-repo release tag            (required, e.g. v0.41.1)
#   PS5_SDK_SHA256   sha256 of ps5-payload-dev.tar.gz    (required)
#   PS5_SDK_CACHE    download directory (actions/cache)  (~/.cache/ps5-sdk)
#   PS5_PAYLOAD_SDK  install location                    (/opt/ps5-payload-sdk)
#
# The tarball is downloaded into PS5_SDK_CACHE unless already there and is
# verified against PS5_SDK_SHA256 every time, cached or not. --dry-run
# verifies a cached tarball if one exists and prints the remaining steps.
set -euo pipefail

DRY=0
[[ "${1:-}" == "--dry-run" ]] && DRY=1

VERSION="${PS5_SDK_VERSION:?PS5_SDK_VERSION is required}"
SHA256="${PS5_SDK_SHA256:?PS5_SDK_SHA256 is required}"
CACHE="${PS5_SDK_CACHE:-${HOME}/.cache/ps5-sdk}"
DEST="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
FILE="ps5-payload-dev.tar.gz"
URL="https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/${VERSION}/${FILE}"
TARBALL="${CACHE}/${VERSION}/${FILE}"

log() { printf '[ps5-sdk] %s\n' "$*"; }
die() { printf '[ps5-sdk] error: %s\n' "$*" >&2; exit 1; }
run() {
    if (( DRY )); then
        printf '[ps5-sdk] would run:'; printf ' %q' "$@"; printf '\n'
    else
        "$@"
    fi
}

if [[ -x "${DEST}/bin/prospero-clang" ]]; then
    log "already installed at ${DEST}"
    exit 0
fi

if [[ ! -f "${TARBALL}" ]]; then
    log "downloading ${URL}"
    run mkdir -p "${CACHE}/${VERSION}"
    run curl -fsSL --retry 3 -o "${TARBALL}.part" "${URL}"
    run mv -f -- "${TARBALL}.part" "${TARBALL}"
else
    log "using cached ${TARBALL}"
fi

if [[ -f "${TARBALL}" ]]; then
    echo "${SHA256}  ${TARBALL}" | sha256sum --check --quiet - \
        || die "checksum mismatch for ${TARBALL} (expected ${SHA256})"
    log "sha256 ok (${SHA256:0:12})"
elif (( ! DRY )); then
    die "${TARBALL} missing after download"
fi

WORK="$(mktemp -d "${RUNNER_TEMP:-/tmp}/ps5sdk.XXXXXX")"
trap 'rm -rf -- "${WORK}"' EXIT
run tar -C "${WORK}" --no-same-owner -xzf "${TARBALL}"
if (( DRY )); then
    log "would move <extracted>/opt/ps5-payload-sdk to ${DEST} (owned by root)"
    exit 0
fi
SRC="${WORK}/opt/ps5-payload-sdk"
[[ -x "${SRC}/bin/prospero-clang" ]] || die "unexpected tarball layout: no opt/ps5-payload-sdk/bin/prospero-clang"
sudo mkdir -p "$(dirname "${DEST}")"
sudo mv -- "${SRC}" "${DEST}"
sudo chown -R root:root "${DEST}"
log "installed ${VERSION} into ${DEST} ($(du -sh "${DEST}" | cut -f1))"
