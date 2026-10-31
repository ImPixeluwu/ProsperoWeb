#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run a build stage, print its wall time and the free disk space, and add a
# line to the GitHub job summary when $GITHUB_STEP_SUMMARY is set.
#
#   scripts/ci/timed.sh "<label>" command [args...]
set -uo pipefail

label="${1:?label}"
shift
start=${SECONDS}
"$@"
rc=$?
elapsed=$((SECONDS - start))
duration="$(printf '%dh%02dm%02ds' $((elapsed / 3600)) $((elapsed % 3600 / 60)) $((elapsed % 60)))"
free="$(df -h --output=avail / | tail -1 | tr -d ' ')"
status=ok
(( rc == 0 )) || status="failed (exit ${rc})"
printf '[ci] %s: %s, %s, %s free on /\n' "${label}" "${duration}" "${status}" "${free}"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf -- '- **%s**: %s, %s, %s free\n' "${label}" "${duration}" "${status}" "${free}" >> "${GITHUB_STEP_SUMMARY}"
fi
exit "${rc}"
