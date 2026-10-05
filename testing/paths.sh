# shellcheck shell=bash
# Directory guards for the container harness.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# run.sh writes into BUILD_ROOT and replaces directories underneath it, so
# the one thing it must never accept is a BUILD_ROOT that really means "/",
# your home directory, or the checkout. Comparing the string the caller typed
# is not enough: "/" and "//" differ as strings, "/tmp/.." and a symlink to
# the checkout both read as somewhere else entirely. Every path is therefore
# canonicalised first and judged afterwards.
#
# Regression tests: testing/test-paths.sh.

# Resolve "..", symlinks and redundant slashes. The path need not exist yet;
# BUILD_ROOT is routinely a directory the harness is about to create.
canonical_path() {
    realpath -m -- "$1"
}

# A directory this harness is allowed to own and overwrite. Takes the name to
# use in messages and the path as the caller gave it; prints the canonical
# path on success.
require_dedicated_dir() {
    local name=$1 given=$2 path repo_real home_real

    case $given in
        /*) ;;
        *) echo "$name must be an absolute path" >&2; return 2 ;;
    esac

    path=$(canonical_path "$given")
    repo_real=$(canonical_path "$HARNESS_REPO")
    home_real=$(canonical_path "${HOME:-/nonexistent}")

    if [[ $path == / ]]; then
        echo "$name must be a dedicated scratch directory, not the filesystem root" >&2
        return 2
    fi
    # One component below "/" is a system-wide directory -- /tmp, /home, /usr.
    # Dedicated scratch lives inside one of those, not instead of one.
    if [[ $path != */*/* ]]; then
        echo "$name must be a dedicated scratch directory, not the top-level $path" >&2
        return 2
    fi
    if [[ $path == "$home_real" ]]; then
        echo "$name must be a dedicated scratch directory, not your home directory" >&2
        return 2
    fi
    # /home/<someone> is a home directory even when it is not yours -- and so
    # is an account whose home is somewhere else entirely, which is why the
    # account database is asked rather than the path being pattern-matched.
    if [[ $path == /home/* && $path != /home/*/* ]]; then
        echo "$name must be a dedicated scratch directory, not the home directory $path" >&2
        return 2
    fi
    if command -v getent >/dev/null 2>&1; then
        local account_home
        while IFS= read -r account_home; do
            [[ -n $account_home ]] || continue
            if [[ $path == "$(canonical_path "$account_home")" ]]; then
                echo "$name must be a dedicated scratch directory, not the home directory of an account on this system ($path)" >&2
                return 2
            fi
        done < <(getent passwd | cut -d: -f6)
    fi
    if [[ $path == "$repo_real" || $path == "$repo_real"/* ]]; then
        echo "$name must be outside the checkout ($path is inside $repo_real)" >&2
        return 2
    fi
    if [[ $repo_real == "$path"/* ]]; then
        echo "$name must not contain the checkout ($repo_real is inside $path)" >&2
        return 2
    fi

    printf '%s\n' "$path"
}

# An existing directory the harness only reads from. Prints its canonical path.
require_existing_dir() {
    local name=$1 given=$2 path

    case $given in
        /*) ;;
        *) echo "$name must be an absolute path" >&2; return 2 ;;
    esac

    path=$(canonical_path "$given")
    if [[ ! -d $path ]]; then
        echo "$name is not a directory: $path" >&2
        return 2
    fi

    printf '%s\n' "$path"
}

# Refuse a read-only input that sits inside a directory the harness will
# overwrite, which would make it both the source and the casualty of the copy.
require_outside() {
    local name=$1 path=$2 owned_name=$3 owned=$4

    if [[ $path == "$owned" || $path == "$owned"/* ]]; then
        echo "$name must not be inside $owned_name, which this harness overwrites ($path)" >&2
        return 2
    fi
}
