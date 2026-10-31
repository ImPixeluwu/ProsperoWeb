#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Free disk space on a GitHub-hosted ubuntu-24.04 runner before the PS5
# build: drop preinstalled SDKs and docker images this project never uses.
#
#   scripts/ci/free-disk.sh [--dry-run]
#
# --dry-run prints what would be removed (and its size, where it exists)
# without touching anything. Never run this on a workstation.
set -euo pipefail

DRY=0
[[ "${1:-}" == "--dry-run" ]] && DRY=1

# Directories of preinstalled toolchains (dotnet, Android SDK, GHC, CodeQL).
DIRS=(
    /usr/share/dotnet
    /usr/local/lib/android
    /opt/ghc
    /usr/local/.ghcup
    /opt/hostedtoolcache/CodeQL
)

log() { printf '[free-disk] %s\n' "$*"; }

log "before:"
df -h /

for d in "${DIRS[@]}"; do
    if [[ ! -e "${d}" ]]; then
        log "absent: ${d}"
        continue
    fi
    if (( DRY )); then
        log "would remove ${d} ($(du -sh "${d}" 2>/dev/null | cut -f1))"
    else
        log "removing ${d}"
        sudo rm -rf -- "${d}"
    fi
done

if command -v docker >/dev/null 2>&1; then
    if (( DRY )); then
        log "would run: docker image prune --all --force"
    else
        log "pruning docker images"
        sudo docker image prune --all --force >/dev/null || log "docker prune failed (ignored)"
    fi
fi

if (( ! DRY )); then
    log "after:"
    df -h /
fi
