#!/usr/bin/env bash
# Fetch the pinned plasma-nm source tree this module's headers come from.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# This is the only step that uses the network, and only when it has to: a tree
# that already matches the pin is left alone.  Nothing is downloaded while the
# module is being built, and nothing is ever downloaded while it runs.
#
# What makes the result trustworthy is not the transport.  A git commit hash
# covers the tree it names, so a fetch of the pinned commit either produces
# that exact tree or fails, whichever mirror served it; the pinned SHA-256
# digests of the four headers actually used are then checked on top, by the
# same CMake code the build uses.  Both checks have to pass before the tree is
# moved into place, so a failed or tampered fetch leaves no usable source tree
# behind at all.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
pin_file=${PLASMA_NM_PIN_FILE:-$repo_root/provenance/plasma-nm-6.7.5.pin}
digest_file=${PLASMA_NM_DIGEST_FILE:-$repo_root/provenance/plasma-nm-6.7.5.sha256}
cmake=${CMAKE:-cmake}
git_bin=${GIT:-git}
# Left in a fetched tree to mark it as this script's to replace.
MARKER=.plasma-nm-source-pin

usage() {
    cat >&2 <<'EOF'
Usage: fetch-plasma-nm-source.sh [destination]
       fetch-plasma-nm-source.sh --verify <directory>

With no argument the pinned revision is fetched into ./plasma-nm-source next
to this checkout, which is where the build looks for it without being told.
--verify only checks a tree that is already there and fetches nothing.
EOF
    exit 2
}

pin_value() {
    local key=$1 value
    value=$(sed -n -E "s/^[[:space:]]*$key[[:space:]]*=[[:space:]]*(.*[^[:space:]])[[:space:]]*\$/\1/p" "$pin_file" | head -n1)
    if [[ -z $value ]]; then
        echo "$pin_file does not set $key" >&2
        return 1
    fi
    printf '%s\n' "$value"
}

verify() {
    local dir=$1
    "$cmake" -DPLASMA_NM_PIN_FILE="$pin_file" \
             -DPLASMA_NM_DIGEST_FILE="$digest_file" \
             -DPLASMA_NM_SOURCE_DIR="$dir" \
             -P "$repo_root/cmake/PlasmaNmSource.cmake"
}

mode=fetch
case ${1:-} in
    --verify)
        [[ $# -eq 2 ]] || usage
        mode=verify
        destination=$2
        ;;
    --help | -h) usage ;;
    -*) usage ;;
    *)
        [[ $# -le 1 ]] || usage
        destination=${1:-$repo_root/plasma-nm-source}
        ;;
esac

case $destination in
    /*) ;;
    *) destination=$PWD/$destination ;;
esac

if [[ $mode == verify ]]; then
    verify "$destination"
    exit
fi

# A tree that already matches is not refetched: the pin is immutable, so there
# is nothing a second fetch could bring.
if verify "$destination" >/dev/null 2>&1; then
    echo "plasma-nm $(pin_value version) source already verified at $destination"
    exit
fi

if [[ -e $destination && ! -d $destination ]]; then
    echo "$destination exists and is not a directory" >&2
    exit 2
fi
# Only a tree this script wrote is ever replaced.  Anything else that happens
# to be there -- your own plasma-nm checkout with work in it, a directory of
# something else entirely -- is left alone and reported, because a verification
# failure is not a licence to delete whatever did not verify.  Re-fetching
# after a re-pin works because the previous fetch left the marker behind.
if [[ -d $destination && -n $(ls -A "$destination" 2>/dev/null) && ! -f $destination/$MARKER ]]; then
    cat >&2 <<EOF
$destination already has files in it that this script did not put there, and
they do not match the pin.  It is not being overwritten.

If it is a plasma-nm checkout of your own, verify it in place instead:
    ${BASH_SOURCE[0]} --verify $destination
Otherwise move or remove it yourself and run this again.
EOF
    exit 2
fi

version=$(pin_value version)
commit=$(pin_value commit)
repository=${PLASMA_NM_REPOSITORY:-$(pin_value repository)}

# Fetched into a sibling directory and moved into place only once both checks
# pass, so an interrupted or wrong fetch cannot leave something the build would
# accept.  mktemp -d next to the destination keeps the move on one filesystem.
parent=$(dirname "$destination")
mkdir -p "$parent"
staging=$(mktemp -d "$parent/.plasma-nm-source.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT

echo "Fetching plasma-nm $version ($commit) from $repository"
"$git_bin" -C "$staging" init -q
"$git_bin" -C "$staging" remote add origin "$repository"
# By hash, not by ref: a ref can be moved, a hash cannot.  --filter=blob:none
# would need a second round trip for every blob that is then read, so the one
# commit is fetched whole and shallow.
"$git_bin" -C "$staging" fetch -q --depth 1 origin "$commit"
"$git_bin" -C "$staging" -c advice.detachedHead=false checkout -q FETCH_HEAD

fetched=$("$git_bin" -C "$staging" rev-parse HEAD)
if [[ $fetched != "$commit" ]]; then
    echo "fetched $fetched, not the pinned $commit" >&2
    exit 1
fi
# git verifies object hashes on receipt, and fsck re-checks the tree that was
# written to disk.
"$git_bin" -C "$staging" fsck --no-dangling --no-progress

verify "$staging"

# Written last, and only to a tree that verified: it is what says a later run
# may replace this directory.
printf '%s\n' "$commit" >"$staging/$MARKER"

rm -rf -- "$destination"
mv -- "$staging" "$destination"
trap - EXIT
echo "plasma-nm $version source verified at $destination"
