# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Regression tests for plasma_nm_upstream_version().
#
#   cmake -P scripts/test-version-parsing.cmake
#
# The ABI gate compares this function's output with the pinned version and
# refuses to build when they differ, so every version string that must not be
# mistaken for the release it is named after is asserted here.

cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/PlasmaNmVersion.cmake)

set(passed 0)
set(failed 0)

function(expect raw expected)
    plasma_nm_upstream_version("${raw}" actual)
    if(actual STREQUAL expected)
        math(EXPR passed "${passed} + 1")
        set(passed "${passed}" PARENT_SCOPE)
        if(expected STREQUAL "")
            message(STATUS "PASS '${raw}' is not a plain release")
        else()
            message(STATUS "PASS '${raw}' -> ${expected}")
        endif()
    else()
        math(EXPR failed "${failed} + 1")
        set(failed "${failed}" PARENT_SCOPE)
        message(STATUS "FAIL '${raw}' -> '${actual}', expected '${expected}'")
    endif()
endfunction()

# What the package managers actually report for a release.
expect("6.7.5" "6.7.5")
expect("6.7.5-1" "6.7.5")
expect("6.7.5-2.1" "6.7.5")
expect("4:6.7.5-1" "6.7.5")
expect("1:6.7.5-0ubuntu3" "6.7.5")
expect("6.7.5-1~bpo12+1" "6.7.5")
expect(" 6.7.5-1 " "6.7.5")
expect("6.7" "6.7")
expect("6" "6")
expect("6.7.5.1" "6.7.5.1")

# Not that release, and must not be reported as it.
expect("6.7.5~rc1-1" "")
expect("6.7.5~beta-1" "")
expect("6.7.5+git20261001-1" "")
expect("6.7.5.dfsg-1" "")
expect("6.7.5a-1" "")
expect("6.7.5rc1" "")
expect("6.7.5.r3.gc74eae7cd16c-1" "")
expect("v6.7.5" "")
expect("" "")
expect("none" "")
expect("6.7.5-1-extra" "")

# A different release is simply a different release; the gate compares, this
# only parses.
expect("6.7.6-1" "6.7.6")

message(STATUS "")
message(STATUS "${passed} passed, ${failed} failed")
if(NOT failed EQUAL 0)
    message(FATAL_ERROR "${failed} version parsing assertions failed")
endif()
