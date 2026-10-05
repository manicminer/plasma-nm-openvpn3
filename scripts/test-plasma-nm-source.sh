#!/usr/bin/env bash
# Regression tests for the pinned-source verification.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# The verification in cmake/PlasmaNmSource.cmake is the whole reason it is safe
# to compile against a library with no versioned ABI, so each way of getting it
# wrong is asserted to fail rather than assumed to.  The tests build their own
# pin and digest fixtures, so they need no network and no plasma-nm; when a
# real pinned tree happens to be present the real pin is checked against it
# too.
set -uo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cmake=${CMAKE:-cmake}
work=$(mktemp -d "${TMPDIR:-/tmp}/plasma-nm-source-test-XXXXXX")
trap 'rm -rf -- "$work"' EXIT

passed=0
failed=0

ok() {
    passed=$((passed + 1))
    printf 'PASS %s\n' "$1"
}

bad() {
    failed=$((failed + 1))
    printf 'FAIL %s\n' "$1"
}

# Run the verification against a tree; prints nothing, returns its status.
verify() {
    local pin=$1 digests=$2 dir=$3
    "$cmake" -DPLASMA_NM_PIN_FILE="$pin" -DPLASMA_NM_DIGEST_FILE="$digests" \
             -DPLASMA_NM_SOURCE_DIR="$dir" -P "$repo_root/cmake/PlasmaNmSource.cmake" >/dev/null 2>&1
}

assert_accepts() {
    local what=$1; shift
    if verify "$@"; then ok "$what"; else bad "$what (expected acceptance, got rejection)"; fi
}

assert_rejects() {
    local what=$1; shift
    if verify "$@"; then bad "$what (expected rejection, got acceptance)"; else ok "$what"; fi
}

# A tree shaped like plasma-nm's, with its own pin and digests. Nothing here
# comes from upstream: the point is the checking, not the content.
make_fixture() {
    local root=$1 version=$2
    mkdir -p "$root/tree/libs/editor/widgets"
    printf 'project(plasma-networkmanagement)\n\nset(PROJECT_VERSION "%s")\n' "$version" >"$root/tree/CMakeLists.txt"
    printf '// one\n' >"$root/tree/libs/editor/vpnuiplugin.h"
    printf '// two\n' >"$root/tree/libs/editor/widgets/settingwidget.h"
    printf 'version = %s\nrepository = https://invent.kde.org/plasma/plasma-nm.git\ncommit = %s\n' \
        "$version" "0000000000000000000000000000000000000000" >"$root/pin"
    (
        cd "$root/tree"
        sha256sum libs/editor/vpnuiplugin.h libs/editor/widgets/settingwidget.h
    ) >"$root/digests"
}

fixture=$work/good
make_fixture "$fixture" 9.9.9
assert_accepts 'an intact tree at the pinned version is accepted' "$fixture/pin" "$fixture/digests" "$fixture/tree"

tampered=$work/tampered
make_fixture "$tampered" 9.9.9
printf '// one\n#define SNEAKY 1\n' >"$tampered/tree/libs/editor/vpnuiplugin.h"
assert_rejects 'a header whose contents changed is rejected' "$tampered/pin" "$tampered/digests" "$tampered/tree"

truncated=$work/truncated
make_fixture "$truncated" 9.9.9
printf '// one' >"$truncated/tree/libs/editor/vpnuiplugin.h"
assert_rejects 'a header that lost its trailing newline is rejected' \
    "$truncated/pin" "$truncated/digests" "$truncated/tree"

removed=$work/removed
make_fixture "$removed" 9.9.9
rm "$removed/tree/libs/editor/widgets/settingwidget.h"
assert_rejects 'a missing header is rejected' "$removed/pin" "$removed/digests" "$removed/tree"

# The digests alone would still match: it is the version that is wrong, and it
# is the version that stands in for the ABI check plasma-nm makes impossible.
wrong_version=$work/wrong-version
make_fixture "$wrong_version" 9.9.9
printf 'project(plasma-networkmanagement)\n\nset(PROJECT_VERSION "9.9.10")\n' >"$wrong_version/tree/CMakeLists.txt"
assert_rejects "a tree of another plasma-nm version is rejected even when the headers match" \
    "$wrong_version/pin" "$wrong_version/digests" "$wrong_version/tree"

not_plasma_nm=$work/not-plasma-nm
make_fixture "$not_plasma_nm" 9.9.9
rm "$not_plasma_nm/tree/CMakeLists.txt"
assert_rejects 'a directory that is not a plasma-nm source tree is rejected' \
    "$not_plasma_nm/pin" "$not_plasma_nm/digests" "$not_plasma_nm/tree"

absent=$work/absent
make_fixture "$absent" 9.9.9
assert_rejects 'a source directory that does not exist is rejected' \
    "$absent/pin" "$absent/digests" "$absent/tree/nowhere"

# An empty digest file would otherwise verify every tree vacuously.
empty_digests=$work/empty-digests
make_fixture "$empty_digests" 9.9.9
printf '# nothing to check\n' >"$empty_digests/digests"
assert_rejects 'a digest file that lists no files is rejected' \
    "$empty_digests/pin" "$empty_digests/digests" "$empty_digests/tree"

missing_digests=$work/missing-digests
make_fixture "$missing_digests" 9.9.9
rm "$missing_digests/digests"
assert_rejects 'a missing digest file is rejected' \
    "$missing_digests/pin" "$missing_digests/digests" "$missing_digests/tree"

unreadable_digests=$work/unreadable-digests
make_fixture "$unreadable_digests" 9.9.9
printf 'not a digest line at all\n' >>"$unreadable_digests/digests"
assert_rejects 'a digest file with an unreadable line is rejected' \
    "$unreadable_digests/pin" "$unreadable_digests/digests" "$unreadable_digests/tree"

broken_pin=$work/broken-pin
make_fixture "$broken_pin" 9.9.9
printf 'this line is not key = value\n' >>"$broken_pin/pin"
assert_rejects 'a pin file that is not key = value is rejected' \
    "$broken_pin/pin" "$broken_pin/digests" "$broken_pin/tree"

missing_pin=$work/missing-pin
make_fixture "$missing_pin" 9.9.9
assert_rejects 'a missing pin file is rejected' \
    "$missing_pin/pin.nope" "$missing_pin/digests" "$missing_pin/tree"

# --verify must report a bad tree through its exit status, since that is how
# both CMake and the workflows find out.
fetch=$repo_root/scripts/fetch-plasma-nm-source.sh
if "$fetch" --verify "$work/tampered/tree" >/dev/null 2>&1; then
    bad 'fetch-plasma-nm-source.sh --verify rejects a tampered tree'
else
    ok 'fetch-plasma-nm-source.sh --verify rejects a tampered tree'
fi

# A destination with somebody else's files in it is never cleared out, whatever
# it looks like. A verification failure says the tree is not the pinned one; it
# does not say anybody wanted it deleted.
# Asserts the guard itself, not just that nothing was lost: with the network
# stubbed out the fetch fails before it would delete anything, so surviving is
# no evidence that the refusal happened. The refusal has its own exit status
# and its own message.
preserved() {
    local what=$1 dir=$2 output status=0
    output=$(GIT=/bin/false "$fetch" "$dir" 2>&1) || status=$?
    if [[ $status -ne 2 ]]; then
        bad "$what (expected exit 2 from the guard, got $status: $output)"
    elif [[ $output != *'is not being overwritten'* ]]; then
        bad "$what (exit 2 but no refusal message: $output)"
    elif [[ ! -f $dir/keep-me ]]; then
        bad "$what (it was deleted)"
    else
        ok "$what"
    fi
}

occupied=$work/occupied
mkdir -p "$occupied"
printf 'mine\n' >"$occupied/keep-me"
preserved 'a non-empty destination this script did not write is refused' "$occupied"

# The regression this guard exists for: a plasma-nm checkout of your own, with
# work in it, that fails verification precisely because of that work.
own_checkout=$work/own-checkout
mkdir -p "$own_checkout/.git" "$own_checkout/libs/editor"
printf 'mine\n' >"$own_checkout/keep-me"
printf 'set(PROJECT_VERSION "6.7.5")\n' >"$own_checkout/CMakeLists.txt"
printf '// my work in progress\n' >"$own_checkout/libs/editor/vpnuiplugin.h"
preserved "a git checkout of your own is refused rather than deleted" "$own_checkout"

# A tree an earlier fetch wrote is this script's to replace, so a re-pin can
# refetch into the same directory. /bin/false stands in for the network, so
# this only reaches the fetch; nothing is deleted before it succeeds.
refetchable=$work/refetchable
mkdir -p "$refetchable"
printf 'stale\n' >"$refetchable/CMakeLists.txt"
printf '0000000000000000000000000000000000000000\n' >"$refetchable/.plasma-nm-source-pin"
refetch_output=$(GIT=/bin/false "$fetch" "$refetchable" 2>&1)
case $refetch_output in
    *'Fetching plasma-nm'*) ok 'a tree an earlier fetch wrote is refetched rather than refused' ;;
    *) bad "a tree an earlier fetch wrote is refetched rather than refused ($refetch_output)" ;;
esac
if [[ -f $refetchable/CMakeLists.txt ]]; then
    ok 'a failed refetch leaves the previous tree in place'
else
    bad 'a failed refetch leaves the previous tree in place'
fi

# The real pin, when a real tree is around to check it against. CI fetches one
# before running this, so this assertion is live there.
real_source=${PLASMA_NM_SOURCE_DIR:-$repo_root/plasma-nm-source}
if [[ -f $real_source/CMakeLists.txt ]]; then
    assert_accepts 'the committed pin verifies the fetched plasma-nm source' \
        "$repo_root/provenance/plasma-nm-6.7.5.pin" \
        "$repo_root/provenance/plasma-nm-6.7.5.sha256" \
        "$real_source"
else
    printf 'SKIP the committed pin verifies the fetched plasma-nm source (no tree at %s)\n' "$real_source"
fi

printf '\n%d passed, %d failed\n' "$passed" "$failed"
[[ $failed -eq 0 ]]
