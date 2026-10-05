#!/usr/bin/env bash
# Build and test this module against a stock plasma-nm, in disposable Docker
# containers.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Never mount a host bus, home directory or secret store.  The checkout is
# mounted read only and is never written to, so nothing here can change the
# tree it is verifying.  Only "images" and "source" use the network, and only
# "images" installs packages, inside Docker.
#
# BUILD_ROOT must be a dedicated scratch directory outside the checkout: this
# script creates it and replaces directories inside it.  See paths.sh for the
# guards and test-paths.sh for their regression tests.
set -euo pipefail
HARNESS_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=paths.sh
. "$(dirname "${BASH_SOURCE[0]}")/paths.sh"

command=${1:-test}
shift || true
case "$command" in
    images|source|build|test) ;;
    *) echo 'Usage: run.sh images|source|build|test [arguments]' >&2; exit 2 ;;
esac

: "${BUILD_ROOT:?Set BUILD_ROOT to a dedicated scratch directory outside the checkout}"
build_root=$(require_dedicated_dir BUILD_ROOT "$BUILD_ROOT")

# Overridable so a second checkout can be verified without overwriting the
# first one's images.
image_build=${IMAGE_BUILD:-plasma-nm-openvpn3-build-env}
image_test=${IMAGE_TEST:-plasma-nm-openvpn3-test-env}
build_type=${BUILD_TYPE:-Debug}
# Two jobs and no swap: enough to build a module this size, little enough to
# leave the machine usable.
jobs=${JOBS:-2}
memory=${MEMORY:-8g}

plasma_nm_source="$build_root/plasma-nm-source"
build_dir="$build_root/build"

# Everything that is not the image build runs unprivileged, with no network, no
# added capabilities and no way to gain any.
contained() {
    docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
        --memory "$memory" --memory-swap "$memory" \
        --user "$(id -u):$(id -g)" \
        -e HOME=/work/home -e XDG_CACHE_HOME=/work/home/.cache -e XDG_RUNTIME_DIR=/work/home/run \
        -v "$HARNESS_REPO:/src:ro" -v "$build_root:/work" \
        "$@"
}

case "$command" in
    images)
        : "${BACKEND_ROOT:?Set BACKEND_ROOT to the companion backend checkout (read only)}"
        backend_root=$(require_existing_dir BACKEND_ROOT "$BACKEND_ROOT")
        require_outside BACKEND_ROOT "$backend_root" BUILD_ROOT/build "$build_dir"
        require_outside BACKEND_ROOT "$backend_root" BUILD_ROOT/plasma-nm-source "$plasma_nm_source"

        # The image build context is a temporary directory this script creates
        # and removes, so nothing under BUILD_ROOT is deleted to make room.
        context=$(mktemp -d "${TMPDIR:-/tmp}/plasma-nm-openvpn3-context-XXXXXX")
        trap 'rm -rf -- "$context"' EXIT
        mkdir -p "$context/backend-src"
        rsync -a --exclude /.git/ --exclude /.hermes/ "$backend_root/" "$context/backend-src/"
        backend_revision=$(git -C "$backend_root" rev-parse HEAD 2>/dev/null || echo unknown)
        cp "$HARNESS_REPO/testing/Dockerfile.test" "$context/Dockerfile.test"

        # BuildKit ignores per-build resource limits, so these are a request
        # and the real bound on the only compilation an image build does is
        # NINJA_JOBS in Dockerfile.test.
        build_limits=(--memory "$memory" --memory-swap "$memory")
        docker build "${build_limits[@]}" \
            -t "$image_build" -f "$HARNESS_REPO/testing/Dockerfile.build" \
            "$HARNESS_REPO/testing"
        docker build "${build_limits[@]}" \
            --build-arg TEST_UID="$(id -u)" \
            --build-arg BASE_IMAGE="$image_build" \
            --build-arg BACKEND_REVISION="$backend_revision" \
            --build-arg NINJA_JOBS="$jobs" \
            -t "$image_test" -f "$context/Dockerfile.test" "$context"
        ;;
    source)
        # The one command with a network, and it only talks to a git remote.
        # The pinned commit hash is what makes the result trustworthy, not the
        # transport; see scripts/fetch-plasma-nm-source.sh.
        mkdir -p "$build_root/home"
        docker run --rm --cap-drop ALL --security-opt no-new-privileges \
            --memory "$memory" --memory-swap "$memory" \
            --user "$(id -u):$(id -g)" \
            -e HOME=/work/home -e XDG_CACHE_HOME=/work/home/.cache \
            -v "$HARNESS_REPO:/src:ro" -v "$build_root:/work" \
            -w /work "$image_build" \
            /src/scripts/fetch-plasma-nm-source.sh /work/plasma-nm-source "$@"
        ;;
    build)
        if [[ ! -f "$plasma_nm_source/CMakeLists.txt" ]]; then
            echo "No plasma-nm source at $plasma_nm_source; run 'run.sh source' first" >&2
            exit 2
        fi
        mkdir -p "$build_dir" "$build_root/home"
        contained -w /work/build "$image_build" sh -ec '
                build_type=$1; jobs=$2; shift 2
                cmake -S /src -B /work/build -G Ninja \
                    -DCMAKE_BUILD_TYPE="$build_type" \
                    -DCMAKE_INSTALL_PREFIX=/usr \
                    -DBUILD_TESTING=ON \
                    -DPLASMA_NM_SOURCE_DIR=/work/plasma-nm-source
                cmake --build /work/build -- -j"$jobs" "$@"
            ' sh "$build_type" "$jobs" "$@"
        ;;
    test)
        mkdir -p "$build_root/home"
        contained -e QT_QPA_PLATFORM=offscreen -w /work/build "$image_test" \
            ctest --output-on-failure "$@"
        ;;
esac
