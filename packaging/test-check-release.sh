#!/usr/bin/env bash
# Regression tests for the release gate.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# The release workflow runs on a tag, which is exactly the circumstance in
# which nobody wants to discover a bug in it. Everything it decides lives in
# check-release.sh so that it can be rehearsed here, with no tag and no
# network, against fixtures this script builds.
set -uo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
script=$repo_root/packaging/check-release.sh
version=$(sed -n -E 's/^project\(plasma-nm-openvpn3 VERSION ([0-9.]+).*/\1/p' "$repo_root/CMakeLists.txt" | head -n1)

work=$(mktemp -d "${TMPDIR:-/tmp}/plasma-nm-openvpn3-release-test-XXXXXX")
trap 'rm -rf -- "$work"' EXIT

passed=0
failed=0
ok() { passed=$((passed + 1)); printf 'PASS %s\n' "$1"; }
bad() { failed=$((failed + 1)); printf 'FAIL %s\n' "$1"; }

# An artifact directory shaped like a real one.
make_artifacts() {
    local dir=$1 for_version=$2
    mkdir -p "$dir"
    printf 'source\n' >"$dir/plasma-nm-openvpn3-$for_version.tar.gz"
    printf 'binary\n' >"$dir/plasma-nm-openvpn3-$for_version-arch-x86_64-plasma-nm-6.7.5.tar.gz"
    (cd "$dir" && sha256sum ./*.tar.gz | sed 's|\./||' >SHA256SUMS)
}

good=$work/good
make_artifacts "$good" "$version"

if "$script" "v$version" "$good" "$work/notes.md" >/dev/null 2>&1; then
    ok "the tag matching the project version, with its artifacts, is accepted"
else
    bad "the tag matching the project version, with its artifacts, is accepted"
fi

for expected in "plasma-nm-openvpn3 $version" "no versioned ABI" "docs/repinning.md" "SHA256SUMS"; do
    if grep -qF "$expected" "$work/notes.md" 2>/dev/null; then
        ok "the notes mention $expected"
    else
        bad "the notes mention $expected"
    fi
done
if grep -qF "plasma-nm-openvpn3-$version-arch-x86_64-plasma-nm-6.7.5.tar.gz" "$work/notes.md" 2>/dev/null; then
    ok "the notes name the binary archive and the plasma-nm it is for"
else
    bad "the notes name the binary archive and the plasma-nm it is for"
fi

refuses() {
    local what=$1; shift
    if "$script" "$@" >/dev/null 2>&1; then
        bad "$what"
    else
        ok "$what"
    fi
}

# The one thing a release must never do quietly.
refuses "a tag that is not the project version is refused" "v0.0.0" "$good"
refuses "a tag with no v is refused" "$version" "$good"
refuses "an empty tag is refused" "" "$good"

missing_source=$work/missing-source
make_artifacts "$missing_source" "$version"
rm "$missing_source/plasma-nm-openvpn3-$version.tar.gz"
refuses "artifacts with no source archive are refused" "v$version" "$missing_source"

missing_binary=$work/missing-binary
make_artifacts "$missing_binary" "$version"
rm "$missing_binary"/plasma-nm-openvpn3-"$version"-arch-*.tar.gz
refuses "artifacts with no binary archive are refused" "v$version" "$missing_binary"

wrong_version=$work/wrong-version
make_artifacts "$wrong_version" 0.0.1
refuses "artifacts built for another version are refused" "v$version" "$wrong_version"

no_sums=$work/no-sums
make_artifacts "$no_sums" "$version"
rm "$no_sums/SHA256SUMS"
refuses "artifacts with no checksum file are refused" "v$version" "$no_sums"

tampered=$work/tampered
make_artifacts "$tampered" "$version"
printf 'not what was built\n' >"$tampered/plasma-nm-openvpn3-$version.tar.gz"
refuses "an artifact that does not match its checksum is refused" "v$version" "$tampered"

refuses "an artifact directory that does not exist is refused" "v$version" "$work/nowhere"

# The release publishes every archive in the directory, so the gate has to
# look at every archive in the directory rather than at whatever the checksum
# file chose to mention.
uncovered=$work/uncovered
make_artifacts "$uncovered" "$version"
grep -v -- "-arch-" "$uncovered/SHA256SUMS" >"$uncovered/SHA256SUMS.new"
mv "$uncovered/SHA256SUMS.new" "$uncovered/SHA256SUMS"
refuses "an archive the checksum file does not cover is refused" "v$version" "$uncovered"

stowaway=$work/stowaway
make_artifacts "$stowaway" "$version"
printf 'from another release\n' >"$stowaway/plasma-nm-openvpn3-9.9.9-arch-x86_64-plasma-nm-6.7.5.tar.gz"
(cd "$stowaway" && sha256sum ./*.tar.gz | sed 's|\./||' >SHA256SUMS)
refuses "an archive of another version left in the directory is refused" "v$version" "$stowaway"

printf '\n%d passed, %d failed\n' "$passed" "$failed"
[[ $failed -eq 0 ]]
