# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Reducing a distribution package version to the upstream release it is.
#
# Separate, pure and script-mode safe so that it can be tested on its own:
# everything about building against a library with no versioned ABI rests on
# this comparison being exact, and "close enough" is how a prerelease or a
# snapshot gets mistaken for the release it is named after.

# The upstream release that @p raw is a package of, or "" when it is not
# exactly one.
#
# Package versions carry an epoch and a distribution revision around the
# upstream version -- 4:6.7.5-1 -- and both are noise here. What is left has
# to be a plain dotted release number: 6.7.5~rc1, 6.7.5+git20261001 and
# 6.7.5.dfsg are all built from sources that are not plasma-nm 6.7.5, so they
# come back empty rather than being rounded down to it.
function(plasma_nm_upstream_version raw out_var)
    set(version "${raw}")
    string(STRIP "${version}" version)

    # Epoch, written the same way by pacman and dpkg.
    if(version MATCHES "^[0-9]+:(.*)$")
        set(version "${CMAKE_MATCH_1}")
    endif()
    # Distribution revision: everything from the last hyphen. rpm's %{VERSION}
    # has none, and an upstream version with a hyphen in it is not a thing KDE
    # releases.
    if(version MATCHES "^(.+)-[^-]*$")
        set(version "${CMAKE_MATCH_1}")
    endif()

    if(version MATCHES "^[0-9]+(\\.[0-9]+)*$")
        set(${out_var} "${version}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()
