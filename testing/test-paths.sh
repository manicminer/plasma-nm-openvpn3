#!/usr/bin/env bash
# Regression tests for run.sh's directory guards.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# run.sh creates BUILD_ROOT and replaces directories inside it, so a BUILD_ROOT
# that resolves to "/", to your home directory or back into the checkout is a
# destructive mistake. These tests pin that down.
#
# Nothing here is allowed to actually delete or copy anything: run.sh is
# invoked with stub docker/rsync/rm/mkdir/cp/git on PATH, which only record
# that they were called. A guard regression therefore shows up as a failing
# assertion, never as damage to the host. The only real filesystem writes are
# inside one mktemp directory that this script creates and removes itself.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
run_sh="$here/run.sh"

work=$(mktemp -d "${TMPDIR:-/tmp}/plasma-nm-openvpn3-path-guard-XXXXXX")
trap 'rm -rf -- "$work"' EXIT
# run.sh makes its own temporary directories and removes them with rm, which
# is stubbed out below; point them inside $work so this script still cleans up.
export TMPDIR="$work"

stubs="$work/stubs"
mkdir -p "$stubs"
for tool in docker rsync rm mkdir cp git; do
    cat >"$stubs/$tool" <<'STUB'
#!/usr/bin/env bash
printf '%s %s\n' "$(basename "$0")" "$*" >>"$STUB_LOG"
exit 0
STUB
    chmod +x "$stubs/$tool"
done

# A synthetic account database, so the home-directory guard is tested against
# known accounts rather than whichever ones this machine happens to have. It
# deliberately includes an account whose home is nowhere near /home. Unlike
# the stubs above it writes no log entry, because the guards call it before
# deciding and a rejected case must still show no side effects.
cat >"$stubs/getent" <<'STUB'
#!/usr/bin/env bash
cat <<'PASSWD'
root:x:0:0::/root:/usr/bin/bash
alice:x:1001:1001::/srv/users/alice:/usr/bin/bash
nobody:x:65534:65534::/:/usr/bin/nologin
PASSWD
STUB
chmod +x "$stubs/getent"

# A valid dedicated scratch directory, and a companion backend checkout.
good_root="$work/scratch/openvpn3-build"
backend="$work/backend-src"
mkdir -p "$good_root" "$backend"
# The build command refuses to run before the pinned source has been fetched,
# which is a different guard; give the accepted cases something to find.
mkdir -p "$good_root/plasma-nm-source"
: >"$good_root/plasma-nm-source/CMakeLists.txt"
# The screenshots command likewise refuses to run before there is anything to
# render, which is a different guard again.
mkdir -p "$good_root/build/bin"
touch "$good_root/build/bin/openvpn3screenshot" "$good_root/build/bin/plasmanetworkmanagement_openvpn3ui.so"
chmod +x "$good_root/build/bin/openvpn3screenshot"
# ... and install-check refuses to run before there is an artifact to install.
mkdir -p "$good_root/package" "$good_root/release-build/bin"
touch "$good_root/package/plasma-nm-openvpn3-0.0.0-arch-x86_64-plasma-nm-0.0.0.tar.gz"
# A symlink alias for the checkout: a string-only guard does not see through it.
ln -s "$repo" "$work/repo-link"

pass=0
fail=0

# run_case <expectation: accept|reject> <description> [VAR=VALUE ...] -- <run.sh arguments>
run_case() {
    local expectation=$1 description=$2
    shift 2
    local -a env_args=()
    while [[ $# -gt 0 && $1 != -- ]]; do
        env_args+=("$1")
        shift
    done
    shift || true

    local log="$work/stub.log"
    : >"$log"
    local rc=0
    local output
    output=$(env -u BUILD_ROOT -u BACKEND_ROOT \
        PATH="$stubs:$PATH" STUB_LOG="$log" \
        "${env_args[@]}" bash "$run_sh" "$@" 2>&1) || rc=$?

    local problem=""
    if [[ $expectation == reject ]]; then
        if [[ $rc -eq 0 ]]; then
            problem="expected a non-zero exit, got 0"
        elif [[ -s $log ]]; then
            # The important half: a rejected directory must be rejected
            # *before* anything touches the filesystem or starts a container.
            problem="rejected, but side effects ran first: $(tr '\n' ';' <"$log")"
        fi
    else
        if [[ $rc -ne 0 ]]; then
            problem="expected success, got exit $rc"
        elif ! grep -q '^docker ' "$log"; then
            problem="accepted, but never reached docker"
        fi
    fi

    if [[ -n $problem ]]; then
        fail=$((fail + 1))
        printf 'FAIL  %s\n        %s\n' "$description" "$problem"
        if [[ -n $output ]]; then
            printf '        output: %s\n' "$(printf '%s' "$output" | tr '\n' ';')"
        fi
    else
        pass=$((pass + 1))
        printf 'ok    %s\n' "$description"
    fi
}

echo "# BUILD_ROOT must name a dedicated directory"
run_case reject 'unset BUILD_ROOT' -- build
run_case reject 'relative BUILD_ROOT' BUILD_ROOT=scratch -- build
run_case reject 'the filesystem root' BUILD_ROOT=/ -- build
run_case reject 'the filesystem root written as //' BUILD_ROOT=// -- build
run_case reject 'the filesystem root written as ///' BUILD_ROOT=/// -- build
run_case reject 'dot-dot that climbs to the root' BUILD_ROOT=/tmp/.. -- build
run_case reject 'dot-dot that climbs to the root, with a trailing slash' BUILD_ROOT=/tmp/../ -- build
run_case reject 'a top-level directory (/tmp)' BUILD_ROOT=/tmp -- build
run_case reject 'a top-level directory (/home)' BUILD_ROOT=/home -- build
run_case reject 'a top-level directory (/usr)' BUILD_ROOT=/usr -- build
run_case reject 'the home directory itself' BUILD_ROOT="$HOME" -- build
run_case reject 'the home directory reached through dot-dot' BUILD_ROOT="$HOME/.cache/.." -- build
run_case reject "another account's home directory" BUILD_ROOT=/home/someone-else -- build
run_case reject "an account whose home is not under /home" BUILD_ROOT=/srv/users/alice -- build
run_case accept "a dedicated directory inside an account's home" BUILD_ROOT=/srv/users/alice/scratch/build -- source

echo
echo "# BUILD_ROOT must not overlap the checkout"
run_case reject 'the checkout itself' BUILD_ROOT="$repo" -- build
run_case reject 'a directory inside the checkout' BUILD_ROOT="$repo/build" -- build
run_case reject 'inside the checkout via dot-dot' BUILD_ROOT="$repo/../$(basename "$repo")/build" -- build
run_case reject 'inside the checkout via a symlink' BUILD_ROOT="$work/repo-link/build" -- build
run_case reject 'a directory that contains the checkout' BUILD_ROOT="$(dirname "$repo")" -- build

echo
echo "# valid dedicated scratch directories keep working"
run_case accept 'a dedicated scratch directory' BUILD_ROOT="$good_root" -- build
run_case accept 'the same directory with a trailing slash' BUILD_ROOT="$good_root/" -- build
run_case accept 'the same directory reached through dot-dot' BUILD_ROOT="$good_root/../openvpn3-build" -- build
run_case accept 'image name overrides' BUILD_ROOT="$good_root" IMAGE_BUILD=a-env IMAGE_TEST=a-test-env -- build
run_case accept 'the test command' BUILD_ROOT="$good_root" -- test -R openvpn3
run_case accept 'the source command' BUILD_ROOT="$good_root" -- source
run_case accept 'the screenshots command' BUILD_ROOT="$good_root" -- screenshots
run_case accept 'the package command' BUILD_ROOT="$good_root" -- package
run_case accept 'the install-check command' BUILD_ROOT="$good_root" -- install-check

echo
echo "# a command with nothing to run yet is refused rather than half-run"
run_case reject 'build before the pinned plasma-nm source was fetched' \
    BUILD_ROOT="$work/scratch/no-source-yet" -- build
run_case reject 'screenshots before anything was built' \
    BUILD_ROOT="$work/scratch/not-built-yet" -- screenshots
run_case reject 'package before the pinned plasma-nm source was fetched' \
    BUILD_ROOT="$work/scratch/not-built-yet" -- package
run_case reject 'install-check before anything was packaged' \
    BUILD_ROOT="$work/scratch/not-packaged-yet" -- install-check
mkdir -p "$work/scratch/two-artifacts/package"
touch "$work/scratch/two-artifacts/package/plasma-nm-openvpn3-0.0.0-arch-x86_64-plasma-nm-0.0.0.tar.gz" \
      "$work/scratch/two-artifacts/package/plasma-nm-openvpn3-0.0.1-arch-x86_64-plasma-nm-0.0.0.tar.gz"
run_case reject 'install-check with more than one artifact to choose from' \
    BUILD_ROOT="$work/scratch/two-artifacts" -- install-check

echo
echo "# companion inputs are validated too"
run_case reject 'images without BACKEND_ROOT' BUILD_ROOT="$good_root" -- images
run_case reject 'images with a BACKEND_ROOT that does not exist' \
    BUILD_ROOT="$good_root" BACKEND_ROOT="$work/absent" -- images
run_case reject 'a BACKEND_ROOT inside the directory build overwrites' \
    BUILD_ROOT="$good_root" BACKEND_ROOT="$good_root/build/backend" -- images
run_case reject 'a BACKEND_ROOT inside the fetched source directory' \
    BUILD_ROOT="$good_root" BACKEND_ROOT="$good_root/plasma-nm-source/backend" -- images
run_case accept 'images with a separate backend checkout' \
    BUILD_ROOT="$good_root" BACKEND_ROOT="$backend" -- images

echo
echo "# an unknown command is still refused"
run_case reject 'an unknown command' BUILD_ROOT="$good_root" -- frobnicate

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]
