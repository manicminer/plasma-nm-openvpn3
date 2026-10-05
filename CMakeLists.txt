# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

cmake_minimum_required(VERSION 3.16)

project(plasma-nm-openvpn3 VERSION 0.1.0)

# The same minimums plasma-nm 6.7.5 itself requires: this module is loaded into
# that build's process, so there is nothing to gain from asking for less.
set(QT_MIN_VERSION "6.10.0")
set(KF6_MIN_VERSION "6.26.0")
set(KDE_COMPILERSETTINGS_LEVEL "5.82")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQtDeclareLoggingCategory)
include(ECMDeprecationSettings)
include(FeatureSummary)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS
    Core
    DBus
    Gui
    Widgets
)
find_package(Qt6Keychain 0.16.0 CONFIG REQUIRED)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED
    CoreAddons
    I18n
    NetworkManagerQt
    WidgetsAddons
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NETWORKMANAGER IMPORTED_TARGET libnm>1.4.0 REQUIRED)

# Deliberately the same relaxations plasma-nm's own build makes, because this
# code is compiled as part of that project upstream and is kept source
# compatible with it: a translation unit that builds here has to build there.
ecm_set_disabled_deprecation_versions(QT 5.15 KF 6.4.0)
add_definitions(-DQT_NO_URL_CAST_FROM_STRING)
remove_definitions(-DQT_NO_CAST_FROM_ASCII -DQT_NO_CAST_FROM_BYTEARRAY)

# A module that resolves every symbol at link time cannot fail to dlopen for a
# symbol that moved, which is the only ABI drift that can be detected at all
# against a library with no versioned ABI. See cmake/PlasmaNmHost.cmake.
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--no-undefined")
string(APPEND CMAKE_MODULE_LINKER_FLAGS " -Wl,--no-undefined")

include(PlasmaNmHost)

add_subdirectory(src)

if(BUILD_TESTING)
    enable_testing()
    add_test(NAME plasma-nm-source-verification
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-plasma-nm-source.sh)
    set_tests_properties(plasma-nm-source-verification PROPERTIES
        ENVIRONMENT "CMAKE=${CMAKE_COMMAND};PLASMA_NM_SOURCE_DIR=${_plasma_nm_source}")
    add_test(NAME plasma-nm-version-parsing
             COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-version-parsing.cmake)
    # The harness guards the directories it overwrites; a regression there is a
    # failed assertion rather than a damaged host. See testing/test-paths.sh.
    add_test(NAME harness-path-guards
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/testing/test-paths.sh)
    # The guards resolve a path before judging it, which is why they cannot
    # see a workflow handing an action a path with a ".." in it; the workflow
    # text is checked separately. See testing/test-workflows.sh.
    add_test(NAME workflow-checks
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/testing/test-workflows.sh)
    # The release gate runs on a tag, which is the worst moment to find a bug
    # in it; this is how it is rehearsed without one.
    add_test(NAME release-gate
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/packaging/test-check-release.sh)
    add_subdirectory(autotests)
endif()

# What the build resolved about its host, for the packaging scripts to put in
# an artifact's provenance rather than guess at afterwards.
configure_file(packaging/build-info.sh.in build-info.sh @ONLY)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
