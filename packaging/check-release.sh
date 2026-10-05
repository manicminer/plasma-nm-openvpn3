#!/usr/bin/env bash
# Check that a tag and the artifacts built for it agree, and write the notes.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Usage: check-release.sh <tag> <artifact directory> [notes file]
#
# This is in a script rather than in the release workflow so that it can be run
# without pushing a tag. A release workflow whose only rehearsal is the release
# is a workflow nobody has tested, and the one thing it must not do is publish
# a tag that says one version with artifacts that say another.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tag=${1:?Usage: check-release.sh <tag> <artifact directory> [notes file]}
artifacts=${2:?Usage: check-release.sh <tag> <artifact directory> [notes file]}
notes=${3:-}

version=$(sed -n -E 's/^project\(plasma-nm-openvpn3 VERSION ([0-9.]+).*/\1/p' "$repo_root/CMakeLists.txt" | head -n1)
[[ -n $version ]] || { echo "could not read the project version from CMakeLists.txt" >&2; exit 1; }

case $tag in
    v*) tagged=${tag#v} ;;
    *) echo "a release tag is vX.Y.Z; $tag is not" >&2; exit 1 ;;
esac

if [[ $tagged != "$version" ]]; then
    echo "tag $tag does not match the project version $version" >&2
    exit 1
fi

source_archive=$artifacts/plasma-nm-openvpn3-$version.tar.gz
[[ -f $source_archive ]] || { echo "no source archive for $version in $artifacts" >&2; exit 1; }

mapfile -t binaries < <(find "$artifacts" -maxdepth 1 -name "plasma-nm-openvpn3-$version-*-*-plasma-nm-*.tar.gz" | LC_ALL=C sort)
[[ ${#binaries[@]} -gt 0 ]] || { echo "no binary archive for $version in $artifacts" >&2; exit 1; }

[[ -f $artifacts/SHA256SUMS ]] || { echo "no SHA256SUMS in $artifacts" >&2; exit 1; }

# The release publishes every archive in this directory, so every archive in it
# has to be one of this version's and has to be covered by the checksum file.
# Verifying only what the manifest happens to list would let a corrupt archive
# through by being left out of it, and an archive for another version through
# by being there at all.
present=$(cd "$artifacts" && find . -maxdepth 1 -name '*.tar.gz' -printf '%P\n' | LC_ALL=C sort)
listed=$(sed -E 's/^[0-9a-f]+[[:space:]]+\*?//' "$artifacts/SHA256SUMS" | LC_ALL=C sort)
if [[ $present != "$listed" ]]; then
    echo "the archives in $artifacts and the ones SHA256SUMS lists are not the same set:" >&2
    diff <(printf '%s\n' "$listed") <(printf '%s\n' "$present") >&2 || true
    exit 1
fi
while IFS= read -r archive; do
    [[ -n $archive ]] || continue
    case $archive in
        "plasma-nm-openvpn3-$version.tar.gz" | "plasma-nm-openvpn3-$version-"*) ;;
        *) echo "$archive is not an archive of version $version, and the release would publish it" >&2; exit 1 ;;
    esac
done <<<"$present"

# Checked where they are, since that is where the names in it are relative to.
(cd "$artifacts" && sha256sum -c SHA256SUMS >/dev/null)

[[ -n $notes ]] || exit 0

{
    cat <<EOF
plasma-nm-openvpn3 $version: an OpenVPN 3 connection editor for an unmodified
distribution plasma-nm.

**A binary archive is good for exactly the plasma-nm named in its filename.**
plasma-nm's editor library has no versioned ABI, so a module built against a
different version loads and then misbehaves rather than failing to load. Build
from the source archive against your own plasma-nm if it is not that version;
see \`docs/repinning.md\`.

It also needs the NetworkManager OpenVPN 3 backend at runtime, and its default
profile storage needs backend support that is not upstream yet; see
\`docs/backend.md\`.

Each binary archive carries its own \`PROVENANCE.txt\` saying what it was built
against, and its own \`SHA256SUMS\`.

## Binary archives

EOF
    for binary in "${binaries[@]}"; do
        printf '* `%s`\n' "$(basename "$binary")"
    done
    cat <<EOF

## Checksums

\`\`\`
$(cat "$artifacts/SHA256SUMS")
\`\`\`
EOF
} >"$notes"
