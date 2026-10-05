#!/usr/bin/env bash
# Build the release artifacts.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Two artifacts, because they are two different promises.
#
# The source tarball is the one that travels: it is the repository at one
# commit and it builds anywhere the pin can be satisfied. Nothing in it is
# specific to a distribution.
#
# The binary tarball is the module and nothing else, and it is good for exactly
# one plasma-nm on one distribution and architecture, which is in its name and
# in its provenance file. plasma-nm's editor library has no versioned ABI, so a
# module built against another version loads and then misbehaves; there is no
# way to make a binary of this portable and no honest way to pretend otherwise.
# It contains no part of plasma-nm and replaces no part of it.
#
# Takes no network. Run inside the build container; see testing/run.sh package.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:?Usage: make-release.sh <build directory> <output directory>}
out_dir=${2:?Usage: make-release.sh <build directory> <output directory>}

# What the build resolved about its host, written by CMake. Sourced rather
# than re-derived, so the provenance says what was actually built against.
# shellcheck disable=SC1091
. "$build_dir/build-info.sh"
version=$PROJECT_VERSION
[[ -n $version ]] || { echo "$build_dir/build-info.sh has no project version" >&2; exit 1; }

commit=$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo unknown)
described=$(git -C "$repo_root" describe --tags --always --dirty 2>/dev/null || echo unknown)

# A release artifact says which commit it is. The source tarball is made with
# git archive, which packs the commit and not the working tree, so a dirty tree
# would produce an archive that is not what was built and a binary whose stated
# provenance is not what it was compiled from. Refused rather than footnoted.
dirty=
if [[ $commit != unknown ]] && ! git -C "$repo_root" diff-index --quiet HEAD -- 2>/dev/null; then
    if [[ -z ${ALLOW_DIRTY_RELEASE:-} ]]; then
        cat >&2 <<EOF
The working tree has uncommitted changes.

The source tarball would be the commit, the binary would be the working tree,
and the provenance would name the commit for both. Commit first, or set
ALLOW_DIRTY_RELEASE=1 for a build whose artifacts say so and must not be
released.
EOF
        exit 2
    fi
    dirty=yes
fi
# The commit's own time, so the same commit packs to the same bytes whenever it
# is packed. No build timestamp goes anywhere near an artifact.
if [[ $commit != unknown ]]; then
    source_date=$(git -C "$repo_root" show -s --format=%ct HEAD)
else
    source_date=0
fi
export SOURCE_DATE_EPOCH=$source_date

module=$build_dir/bin/plasmanetworkmanagement_openvpn3ui.so
[[ -f $module ]] || { echo "no built module at $module; build first" >&2; exit 2; }

plugin_subdir=$KDE_INSTALL_PLUGINDIR/plasma/network/vpn
host_version=${PLASMA_NM_HOST_VERSION_RESOLVED:-unknown}
host_package=${PLASMA_NM_HOST_PACKAGE_VERSION:-unknown}
distro=${DISTRO_ID:-$( (. /etc/os-release 2>/dev/null && echo "$ID") || echo unknown)}
arch=${ARCH:-$(uname -m)}
image_id=${BUILD_IMAGE_ID:-unknown}

mkdir -p "$out_dir"
rm -rf "${out_dir:?}/staging"
mkdir -p "$out_dir/staging"

# --- the source tarball -----------------------------------------------------

source_name=plasma-nm-openvpn3-$version
if [[ $commit != unknown ]]; then
    git -C "$repo_root" archive --format=tar --prefix="$source_name/" HEAD \
        | gzip -9n >"$out_dir/$source_name.tar.gz"
else
    echo "not a git checkout; skipping the source tarball" >&2
fi

# --- the binary tarball -----------------------------------------------------

binary_name=plasma-nm-openvpn3-$version-$distro-$arch-plasma-nm-$host_version
root=$out_dir/staging/$binary_name
mkdir -p "$root/usr/$plugin_subdir"
install -m 0644 "$module" "$root/usr/$plugin_subdir/"

# What this binary is, and what it is not. Read by anyone who finds the file
# without the page it was downloaded from.
cat >"$root/PROVENANCE.txt" <<EOF
plasma-nm-openvpn3 $version -- binary module

  source commit     $commit ($described)${dirty:+
  NOT A RELEASE     built from a dirty working tree; the commit above is not
                    what this was compiled from}
  built against     plasma-nm $host_version (package version $host_package)
  distribution      $distro
  architecture      $arch
  build image       $image_id
  plasma-nm pin     $PLASMA_NM_PIN_VERSION $PLASMA_NM_PIN_COMMIT
  linked against    $PLASMA_NM_EDITOR_LIBRARY
  compiler          $CMAKE_CXX_COMPILER_ID $CMAKE_CXX_COMPILER_VERSION
  Qt / KF           $QT_VERSION / $KF_VERSION

This archive contains one file: the OpenVPN 3 VPN plugin for plasma-nm's
connection editor. It contains no part of plasma-nm, replaces no part of
plasma-nm, and installs nothing that any distribution package owns.

It is good for plasma-nm $host_version on $distro/$arch and for nothing else.
plasma-nm's editor library has no SOVERSION and no ABI tag, so a module built
against a different version of it will load and then misbehave or crash inside
the connection editor rather than failing to load. If your plasma-nm is not
$host_version, build from source against yours; see docs/repinning.md in the
source tarball.

It also needs the NetworkManager OpenVPN 3 backend at runtime, which provides
the org.freedesktop.NetworkManager.openvpn3 service. Without it the connection
type does not appear.

Install:

    sudo tar -C / -xvf $binary_name.tar.gz --strip-components=1 \\
        $binary_name/usr

Remove:

    sudo rm /usr/$plugin_subdir/plasmanetworkmanagement_openvpn3ui.so

Verify before installing:

    sha256sum -c SHA256SUMS
EOF
cp "$repo_root/LICENSES/GPL-2.0-only.txt" "$repo_root/LICENSES/GPL-3.0-only.txt" \
   "$repo_root/LICENSES/LicenseRef-KDE-Accepted-GPL.txt" "$root/"
(
    cd "$root"
    find . -type f ! -name SHA256SUMS -printf '%P\n' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS
)
tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$source_date" --format=gnu \
    -C "$out_dir/staging" -cf - "$binary_name" | gzip -9n >"$out_dir/$binary_name.tar.gz"
rm -rf "${out_dir:?}/staging"

# --- what was produced ------------------------------------------------------

(
    cd "$out_dir"
    find . -maxdepth 1 -type f -name '*.tar.gz' -printf '%P\n' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS
)
cat "$out_dir/SHA256SUMS"
