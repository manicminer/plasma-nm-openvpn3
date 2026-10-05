#!/usr/bin/env bash
# Regression tests for what the workflows hand to the actions they call.
#
# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Run 37331953615 built, tested, packaged and install-checked everything, and
# then failed on the upload: BUILD_ROOT was "<workspace>/../openvpn3-build",
# which run.sh canonicalises without complaint and which
# actions/upload-artifact rejects as a string, however it resolves. The
# harness's own guards cannot catch that -- by the time they see a path they
# have already resolved it -- so the text of the workflow is checked here.
#
# The other half is softness: a step that is allowed to fail turns a missing
# OpenVPN 3 backend into skipped importer tests and a green run, which is the
# one outcome a release must never be built from.
#
# Nothing here starts a container, uses the network or writes anywhere but one
# mktemp directory it removes itself.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
workflows="$repo/.github/workflows"

# What a GitHub-hosted runner really expands these to, so a value written as a
# context expression can be judged as the path the action will be handed.
runner_temp=/home/runner/work/_temp
runner_workspace=/home/runner/work/plasma-nm-openvpn3/plasma-nm-openvpn3

work=$(mktemp -d "${TMPDIR:-/tmp}/plasma-nm-openvpn3-workflow-XXXXXX")
trap 'rm -rf -- "$work"' EXIT

pass=0
fail=0

ok() { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
bad() {
    fail=$((fail + 1))
    printf 'FAIL  %s\n        %s\n' "$1" "$(printf '%s' "$2" | tr '\n' ';')"
}

# The checks below are about what a workflow *does*, and a comment is not
# that: this file's own prose quotes both "/../" and "continue-on-error", and
# so does build.yml's. Judge the YAML with the comments taken out. A "#" inside
# a quoted value is cut too, which would make a path containing one fail here
# -- the harmless direction, and the alternative is a YAML parser this does not
# have in the container it runs in.
shopt -s nullglob
files=("$workflows"/*.yml "$workflows"/*.yaml)
shopt -u nullglob
if [[ ${#files[@]} -eq 0 ]]; then
    echo "No workflows found in $workflows" >&2
    exit 1
fi
for file in "${files[@]}"; do
    sed -E 's/(^|[[:space:]])#.*$//' "$file" >"$work/$(basename "$file").code"
done

echo '# no workflow hands an action a path containing ".."'
for file in "${files[@]}"; do
    name=${file#"$repo/"}
    code="$work/$(basename "$file").code"
    if offenders=$(grep -nE '(^|[^[:alnum:]_.])\.\.(/|$)' "$code"); then
        bad "$name has no \"..\" path segment" \
            "actions/upload-artifact rejects these as strings: $offenders"
    else
        ok "$name has no \"..\" path segment"
    fi
done

echo
echo '# nothing in CI is allowed to fail softly'
for file in "${files[@]}"; do
    name=${file#"$repo/"}
    code="$work/$(basename "$file").code"
    if offenders=$(grep -nE '^[[:space:]]*continue-on-error:' "$code"); then
        bad "$name has no continue-on-error" \
            "a step that fails softly makes a failed run look like a lesser one: $offenders"
    else
        ok "$name has no continue-on-error"
    fi
done

# Stand in for "$NAME", "${NAME}" and "${{ context.name }}" with what a runner
# really sets -- and only for these names, matched whole: $RUNNER_TEMP_OTHER is
# not RUNNER_TEMP, and a value mentioning anything this test does not know is
# reported rather than quietly half-resolved into something that passes.
#
# Where the value sits decides what may expand in it. A "run:" script is run by
# a shell, so "$RUNNER_TEMP" there is a path; a YAML env value is not, so the
# same spelling would reach the harness with the dollar sign still in it.
resolve() {
    local form=$1 value=$2 out='' name
    while [[ $value =~ ^([^$]*)\$(\{\{[[:space:]]*([A-Za-z_][A-Za-z_0-9.]*)[[:space:]]*\}\}|\{([A-Za-z_][A-Za-z_0-9]*)\}|([A-Za-z_][A-Za-z_0-9]*))(.*)$ ]]; do
        out+=${BASH_REMATCH[1]}
        name=${BASH_REMATCH[3]}${BASH_REMATCH[4]}${BASH_REMATCH[5]}
        if [[ -z ${BASH_REMATCH[3]} && $form == yaml ]]; then
            printf 'nothing expands $%s in a YAML value; it would arrive literally\n' "$name"
            return 1
        fi
        case $name in
            runner.temp|RUNNER_TEMP) out+=$runner_temp ;;
            github.workspace|GITHUB_WORKSPACE) out+=$runner_workspace ;;
            *)
                printf 'this test does not know what %s expands to; teach it\n' "$name"
                return 1
                ;;
        esac
        value=${BASH_REMATCH[6]}
    done
    if [[ $value == *'$'* ]]; then
        printf 'cannot resolve the expansion in "%s"\n' "$value"
        return 1
    fi
    printf '%s%s\n' "$out" "$value"
}

# The two spellings BUILD_ROOT may arrive by, pinned exactly: an env entry, or
# the line that exports it to the steps that follow. Prints
# "<line>\t<form>\t<value>" per assignment, and reports any other line that
# assigns to BUILD_ROOT as "unknown" rather than passing over it -- a second,
# unrecognised assignment is how a judged value gets quietly replaced, and
# "echo 'BUILD_ROOT=$RUNNER_TEMP/x'" in single quotes exports a literal dollar
# sign and is not the same thing as the double-quoted line.
assignments_in() {
    local lineno=0 line value
    while IFS= read -r line; do
        lineno=$((lineno + 1))
        if [[ $line =~ ^[[:space:]]*BUILD_ROOT:[[:space:]]*(.+)$ ]]; then
            value=${BASH_REMATCH[1]}
            while [[ $value == *[[:space:]] ]]; do value=${value%[[:space:]]}; done
            if [[ $value == \"*\" || $value == \'*\' ]]; then
                value=${value:1:-1}
            fi
            printf '%s\tyaml\t%s\n' "$lineno" "$value"
        elif [[ $line =~ ^[[:space:]]*echo[[:space:]]+\"BUILD_ROOT=([^\"]*)\"[[:space:]]*\>\>[[:space:]]*\"\$GITHUB_ENV\"[[:space:]]*$ ]]; then
            printf '%s\tshell\t%s\n' "$lineno" "${BASH_REMATCH[1]}"
        elif [[ $line =~ BUILD_ROOT[[:space:]]*[:=] ]]; then
            printf '%s\tunknown\t%s\n' "$lineno" "$line"
        fi
    done <"$1"
    return 0
}

echo
echo '# BUILD_ROOT is set before it is used, canonical, and accepted by the guards'
total=0
for file in "${files[@]}"; do
    label=${file#"$repo/"}
    code="$work/$(basename "$file").code"
    first_mention=$({ grep -n 'BUILD_ROOT' "$code" || true; } | head -n1 | cut -d: -f1)
    # A workflow with nothing to say about BUILD_ROOT has nothing to get wrong.
    [[ -n $first_mention ]] || continue

    mapfile -t assignments < <(assignments_in "$code")
    recognised=0
    for entry in "${assignments[@]}"; do
        [[ ${entry#*$'\t'} == unknown* ]] || recognised=$((recognised + 1))
    done
    if [[ $recognised -eq 0 ]]; then
        bad "$label sets the BUILD_ROOT it uses" \
            "line $first_mention uses BUILD_ROOT and nothing in this file sets it in a spelling this test recognises"
        continue
    fi

    # Line order, not a scope analysis: an env entry is in scope from the top
    # and an export reaches the steps after it, so a use above the line that
    # sets it is a mistake either way. An actually unset BUILD_ROOT is a loud
    # failure anyway -- run.sh refuses to do anything without one -- which is
    # why this only has to catch the ordering.
    first_set=${assignments[0]%%$'\t'*}
    if [[ $first_mention -lt $first_set ]]; then
        bad "$label sets BUILD_ROOT before anything uses it" \
            "line $first_mention uses it, line $first_set sets it"
    else
        ok "$label sets BUILD_ROOT at line $first_set, before anything uses it"
    fi

    for entry in "${assignments[@]}"; do
        lineno=${entry%%$'\t'*}
        rest=${entry#*$'\t'}
        form=${rest%%$'\t'*}
        value=${rest#*$'\t'}
        total=$((total + 1))
        if [[ $form == unknown ]]; then
            bad "$label line $lineno sets BUILD_ROOT in a spelling this test can judge" \
                "it does not: $value"
            continue
        fi
        if ! expanded=$(resolve "$form" "$value"); then
            bad "$label line $lineno sets a BUILD_ROOT this test can resolve" "$expanded"
            continue
        fi

        # The harness's guards, asked about the runner's layout rather than
        # this machine's: a dedicated directory, outside the checkout, not a
        # home directory.
        rc=0
        canonical=$(HARNESS_REPO="$runner_workspace" HOME=/home/runner \
            bash -c '. "$1/paths.sh"; require_dedicated_dir BUILD_ROOT "$2"' \
            bash "$here" "$expanded" 2>&1) || rc=$?
        if [[ $rc -ne 0 ]]; then
            bad "$label line $lineno sets a BUILD_ROOT the harness accepts" "$canonical"
        elif [[ $canonical != "$expanded" ]]; then
            # Accepted, but only after being resolved -- which is exactly the
            # difference that passed every build step and failed the upload.
            bad "$label line $lineno sets a BUILD_ROOT that is already canonical" \
                "\"$expanded\" resolves to \"$canonical\""
        else
            ok "$label line $lineno sets BUILD_ROOT=$value ($expanded)"
        fi
    done
done
if [[ $total -eq 0 ]]; then
    bad 'a workflow sets BUILD_ROOT' \
        'none does, so nothing above was checked; if it moved, point this test at it'
fi

echo
echo '# actionlint'
if command -v actionlint >/dev/null 2>&1; then
    rc=0
    output=$(cd "$repo" && actionlint 2>&1) || rc=$?
    if [[ $rc -eq 0 ]]; then
        ok "actionlint ($(actionlint --version | head -n1)) reports nothing"
    else
        bad 'actionlint reports nothing' "$output"
    fi
elif [[ -n ${REQUIRE_ACTIONLINT:-} ]]; then
    # The CI lint job sets this: a linter that quietly is not there lints
    # nothing, which is the same failure mode as a step allowed to fail.
    bad 'actionlint is available' 'REQUIRE_ACTIONLINT is set but actionlint is not on PATH'
else
    printf 'skip  actionlint is not installed; the CI lint job runs it with REQUIRE_ACTIONLINT=1\n'
fi

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]
