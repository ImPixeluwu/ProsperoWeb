#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Print release notes for a tag: the matching section of CHANGELOG.md if
# there is one, else a short template.
#
#   scripts/ci/release-notes.sh <tag> <product-name> [CHANGELOG.md]
#
# A section matches when its "## " heading contains the version as a word,
# with or without the leading "v" ("## [0.2.0] - 2026-11-01", "## v0.2.0").
# The section runs up to the next "## " heading.
set -euo pipefail

TAG="${1:?tag}"
PRODUCT="${2:?product name}"
CHANGELOG="${3:-CHANGELOG.md}"
VERSION="${TAG#v}"

section=""
if [[ -f "${CHANGELOG}" ]]; then
    section="$(awk -v ver="${VERSION}" '
        BEGIN {
            gsub(/\./, "\\.", ver)
            re = "(^|[^0-9A-Za-z.])v?" ver "([^0-9A-Za-z.]|$)"
        }
        /^## / { if (found) exit; if ($0 ~ re) { found = 1; next } }
        found { print }
    ' "${CHANGELOG}")"
fi

if [[ -n "${section//[[:space:]]/}" ]]; then
    printf '%s\n' "${section}"
    exit 0
fi

cat <<NOTES
${PRODUCT} ${TAG}.

Assets:

- \`${PRODUCT}-${TAG}-data.zip\`: the console data directory and the websrv launcher tile. Unpack it and copy both folders under \`/data/\` over FTP; \`README-install.txt\` inside has the steps.
- The native title zip, when attached: an installable home-screen tile (experimental).

The PS5 build is not verified on hardware yet; see \`docs/console-verification.md\`.
NOTES
