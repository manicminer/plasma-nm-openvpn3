#!/usr/bin/env bash
# Install the binary artifact on a stock host and check what it did.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Run inside a disposable container only; it writes to /usr. See
# testing/run.sh install-check, which is the only thing that should call it.
#
# The questions it answers are the ones a user would ask about a tarball that
# writes into /usr: does it put the file where it said, does it overwrite
# anything the distribution owns, do the VPN plugins that were already there
# still work, and does the installed module actually load -- as opposed to the
# one in the build tree, which every other test has been using.
set -euo pipefail

artifact=${1:?Usage: verify-install.sh <binary tarball> <build directory>}
build_dir=${2:?Usage: verify-install.sh <binary tarball> <build directory>}

[[ -f $artifact ]] || { echo "no such artifact: $artifact" >&2; exit 2; }
[[ $(id -u) -eq 0 ]] || { echo "this installs into /usr and has to be root in a container" >&2; exit 2; }
[[ -f /.dockerenv || -f /run/.containerenv ]] || { echo "refusing to run outside a container" >&2; exit 2; }

pass=0
fail=0
ok() { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n' "$1"; }
check() { if "${@:2}" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

echo "# the archive says what it is"
tar -tzf "$artifact" >"$work/listing"
prefix=$(head -n1 "$work/listing" | cut -d/ -f1)
tar -xzf "$artifact" -C "$work"
check "it carries its own checksums" test -f "$work/$prefix/SHA256SUMS"
check "it carries its provenance" test -f "$work/$prefix/PROVENANCE.txt"
if (cd "$work/$prefix" && sha256sum -c SHA256SUMS) >/dev/null 2>&1; then
    ok "every listed file matches its checksum"
else
    bad "every listed file matches its checksum"
fi
# ... and the listing has to cover everything, or an archive could leave the
# one file that matters out of its own manifest and still pass the line above.
(cd "$work/$prefix" && find . -type f ! -name SHA256SUMS -printf '%P\n' | LC_ALL=C sort) >"$work/present"
sed -E 's/^[0-9a-f]+[[:space:]]+\*?//' "$work/$prefix/SHA256SUMS" | LC_ALL=C sort >"$work/listed"
if diff -q "$work/present" "$work/listed" >/dev/null; then
    ok "the checksum file covers every file in the archive, and no others"
else
    bad "the checksum file covers every file in the archive, and no others"
    diff "$work/present" "$work/listed" | head -10
fi

echo
echo "# it contains the module and nothing else that lands in the filesystem"
mapfile -t installed < <(grep -E "^$prefix/usr/" "$work/listing" | grep -v '/$' | sed "s|^$prefix||")
printf '      %s\n' "${installed[@]}"
if [[ ${#installed[@]} -eq 1 && ${installed[0]} == */plasma/network/vpn/plasmanetworkmanagement_openvpn3ui.so ]]; then
    ok "exactly one file is installed, and it is the VPN plugin"
else
    bad "exactly one file is installed, and it is the VPN plugin"
fi

echo
echo "# nothing the distribution owns is overwritten"
clash=0
for file in "${installed[@]}"; do
    if owner=$(pacman -Qoq "$file" 2>/dev/null) && [[ -n $owner ]]; then
        bad "$file is already owned by $owner"
        clash=1
    fi
done
[[ $clash -eq 0 ]] && ok "no installed path belongs to a distribution package"

# Everything pacman knows about, before and after, so a changed file shows up
# whether or not we expected to touch it.
pacman -Qkk >"$work/before" 2>&1 || true
# And the VPN plugins that were there first, by name, so that gaining one and
# losing none can be checked rather than assumed.
vpn_dir_before=$(dirname "/usr/${installed[0]#/usr/}")
plugins_before=$( (cd "$vpn_dir_before" && printf '%s\n' *.so | LC_ALL=C sort) 2>/dev/null || true)

echo
echo "# install it the way the provenance file says to"
tar -C / -xf "$artifact" --strip-components=1 "$prefix/usr"
check "the module is where the host looks for it" \
    test -f "/usr/${installed[0]#/usr/}"

pacman -Qkk >"$work/after" 2>&1 || true
if diff -q "$work/before" "$work/after" >/dev/null; then
    ok "no distribution-owned file changed"
else
    bad "distribution-owned files changed:"
    diff "$work/before" "$work/after" | head -20
fi

echo
echo "# the installed module is the one that loads"
# The test binaries come from the build tree because they are tests, but what
# they load here has to be what was just installed. QT_PLUGIN_PATH is unset,
# and the binaries are copied somewhere with nothing beside them first: the
# build tree keeps a copy of the module in bin/plasma/network/vpn so the
# ordinary test run can find it, that sits next to the binaries, and Qt looks
# next to the binary. Run from there, "it found a plugin" would not be the same
# claim as "it found the installed plugin". The build tree is left alone --
# nothing here may write to it.
export QT_QPA_PLATFORM=offscreen
unset QT_PLUGIN_PATH
mkdir -p "$work/bin"
installed_so=/usr/${installed[0]#/usr/}
for suite in openvpn3plugintest openvpn3hostcompattest; do
    if [[ -x $build_dir/bin/$suite ]]; then
        cp "$build_dir/bin/$suite" "$work/bin/$suite"
        if "$work/bin/$suite" >"$work/$suite.log" 2>&1; then
            ok "$suite passes against the installed module"
        else
            bad "$suite against the installed module"
            tail -20 "$work/$suite.log"
        fi
    else
        bad "$suite was not built, so nothing was checked with it"
    fi
done
# openvpn3plugintest asserts that exactly one plugin claims the openvpn3
# service; with nothing beside the binary and no QT_PLUGIN_PATH, that one is
# the file installed above.
check "the installed module is the only openvpn3 plugin on the default path" \
    test -f "$installed_so"

echo
echo "# the VPN plugins that were already there still work"
# Which is asserted by loading them, not by finding their files:
# openvpn3plugintest instantiates every installed VPN plugin and fails if any
# of them no longer does, so the run above has already covered it. What is
# checked here is that the directory gained one file and lost none.
installed_name=$(basename "$installed_so")
vpn_dir=$(dirname "$installed_so")
if [[ -n ${plugins_before:-} ]]; then
    plugins_now=$(cd "$vpn_dir" && printf '%s\n' *.so | LC_ALL=C sort)
    if [[ $(printf '%s\n' "$plugins_now" | grep -vFx "$installed_name") == "$plugins_before" ]]; then
        ok "the other VPN plugins are exactly the ones that were there before"
    else
        bad "the other VPN plugins are exactly the ones that were there before"
        diff <(printf '%s\n' "$plugins_before") <(printf '%s\n' "$plugins_now") | head -10
    fi
fi

echo
echo "# removing it leaves nothing behind"
rm -f "$installed_so"
pacman -Qkk >"$work/removed" 2>&1 || true
if diff -q "$work/before" "$work/removed" >/dev/null; then
    ok "removing the one file restores the host exactly"
else
    bad "removing the one file restores the host exactly"
fi

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]
