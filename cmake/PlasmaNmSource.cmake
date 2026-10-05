# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Reading and verifying a pinned plasma-nm source tree.
#
# Usable two ways, so that the digests are checked by exactly one
# implementation wherever the source came from:
#
#   include(PlasmaNmSource)                 from the project's CMakeLists
#   cmake -DPLASMA_NM_PIN_FILE=... \         from scripts/fetch-plasma-nm-source.sh
#         -DPLASMA_NM_DIGEST_FILE=... \
#         -DPLASMA_NM_SOURCE_DIR=... -P cmake/PlasmaNmSource.cmake

# Parse a pin file into ${prefix}_VERSION, _REPOSITORY, _MIRROR, _COMMIT,
# _TAG and _LICENSE in the caller's scope.
function(plasma_nm_read_pin pin_file prefix)
    if(NOT EXISTS "${pin_file}")
        message(FATAL_ERROR "plasma-nm pin file is missing: ${pin_file}")
    endif()
    file(STRINGS "${pin_file}" lines)
    foreach(key VERSION REPOSITORY MIRROR COMMIT TAG LICENSE)
        set(${prefix}_${key} "" PARENT_SCOPE)
    endforeach()
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*#" OR line MATCHES "^[ \t]*$")
            continue()
        endif()
        if(NOT line MATCHES "^[ \t]*([A-Za-z_]+)[ \t]*=[ \t]*(.*[^ \t])[ \t]*$")
            message(FATAL_ERROR "plasma-nm pin file has a line that is neither a comment nor key = value: ${line}")
        endif()
        string(TOUPPER "${CMAKE_MATCH_1}" key)
        set(${prefix}_${key} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    endforeach()
endfunction()

# The upstream version of a plasma-nm source tree, or "" when the directory is
# not one. Read from the one place upstream states it.
function(plasma_nm_source_version source_dir out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT EXISTS "${source_dir}/CMakeLists.txt")
        return()
    endif()
    file(STRINGS "${source_dir}/CMakeLists.txt" version_lines REGEX "^[ \t]*set\\(PROJECT_VERSION ")
    foreach(line IN LISTS version_lines)
        if(line MATCHES "set\\(PROJECT_VERSION[ \t]+\"([^\"]+)\"\\)")
            set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# Check a plasma-nm source tree against a digest file and an expected version.
#
# Sets ${result_var} to TRUE or FALSE and ${message_var} to a single line
# saying what was wrong, so a caller can turn it into a fatal error of its own
# wording. Fails closed: an unreadable tree, a missing header, a changed
# header and the wrong upstream version are all failures.
function(plasma_nm_verify_source source_dir digest_file expected_version result_var message_var)
    set(${result_var} FALSE PARENT_SCOPE)
    if(NOT IS_DIRECTORY "${source_dir}")
        set(${message_var} "not a directory: ${source_dir}" PARENT_SCOPE)
        return()
    endif()
    if(NOT EXISTS "${digest_file}")
        set(${message_var} "digest file is missing: ${digest_file}" PARENT_SCOPE)
        return()
    endif()

    plasma_nm_source_version("${source_dir}" found_version)
    if(found_version STREQUAL "")
        set(${message_var} "${source_dir} is not a plasma-nm source tree: no PROJECT_VERSION in its CMakeLists.txt" PARENT_SCOPE)
        return()
    endif()
    if(NOT found_version STREQUAL expected_version)
        set(${message_var} "${source_dir} is plasma-nm ${found_version}, not the pinned ${expected_version}" PARENT_SCOPE)
        return()
    endif()

    file(STRINGS "${digest_file}" digest_lines)
    set(checked 0)
    foreach(line IN LISTS digest_lines)
        if(line MATCHES "^[ \t]*#" OR line MATCHES "^[ \t]*$")
            continue()
        endif()
        if(NOT line MATCHES "^([0-9a-f]+)[ \t]+[*]?(.+)$")
            set(${message_var} "digest file ${digest_file} has an unreadable line: ${line}" PARENT_SCOPE)
            return()
        endif()
        set(expected_digest "${CMAKE_MATCH_1}")
        set(relative "${CMAKE_MATCH_2}")
        if(NOT EXISTS "${source_dir}/${relative}")
            set(${message_var} "${relative} is missing from ${source_dir}" PARENT_SCOPE)
            return()
        endif()
        file(SHA256 "${source_dir}/${relative}" actual_digest)
        if(NOT actual_digest STREQUAL expected_digest)
            set(${message_var} "${relative} does not match the pinned digest (expected ${expected_digest}, got ${actual_digest})" PARENT_SCOPE)
            return()
        endif()
        math(EXPR checked "${checked} + 1")
    endforeach()
    if(checked EQUAL 0)
        set(${message_var} "digest file ${digest_file} lists no files" PARENT_SCOPE)
        return()
    endif()

    set(${result_var} TRUE PARENT_SCOPE)
    set(${message_var} "${checked} pinned plasma-nm ${expected_version} headers verified" PARENT_SCOPE)
endfunction()

# Script mode: verify and exit non-zero on failure.
if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_CURRENT_LIST_FILE STREQUAL CMAKE_SCRIPT_MODE_FILE)
    foreach(required PLASMA_NM_PIN_FILE PLASMA_NM_DIGEST_FILE PLASMA_NM_SOURCE_DIR)
        if(NOT DEFINED ${required})
            message(FATAL_ERROR "-D${required}=... is required in script mode")
        endif()
    endforeach()
    plasma_nm_read_pin("${PLASMA_NM_PIN_FILE}" PIN)
    plasma_nm_verify_source("${PLASMA_NM_SOURCE_DIR}" "${PLASMA_NM_DIGEST_FILE}" "${PIN_VERSION}" ok reason)
    if(NOT ok)
        message(FATAL_ERROR "plasma-nm source verification failed: ${reason}")
    endif()
    message(STATUS "${reason}")
endif()
