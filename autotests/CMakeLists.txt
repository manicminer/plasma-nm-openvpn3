# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

include(ECMAddTests)

find_package(Qt6 ${QT_MIN_VERSION} NO_MODULE REQUIRED Test)

# Where the synthetic example.org fixtures live. No real key, certificate or
# credential is in there; the hosts are all example.org/example.net.
add_definitions(-DOPENVPN3_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\")

ecm_add_tests(
    openvpn3profiletest.cpp
    openvpn3storagetest.cpp
    openvpn3importtest.cpp
    NAME_PREFIX openvpn3-
    LINK_LIBRARIES Qt::Test plasmanetworkmanagement_openvpn3_core
)

ecm_add_tests(
    openvpn3directivestest.cpp
    openvpn3widgettest.cpp
    openvpn3authtest.cpp
    NAME_PREFIX openvpn3-
    GUI
    LINK_LIBRARIES Qt::Test Qt::Widgets Qt::DBus KF6::CoreAddons plasmanetworkmanagement_openvpn3_core
)

# Not a test: renders the editor for review. See openvpn3screenshot.cpp.
add_executable(openvpn3screenshot openvpn3screenshot.cpp)
target_link_libraries(openvpn3screenshot Qt::Widgets plasmanetworkmanagement_openvpn3_core)

# These two load the built module out of its shared object, so they need it
# built and on the plugin path. Everything else about the host -- plasma-nm's
# editor library, the distribution's other VPN plugins -- is the installed one.
ecm_add_test(
    openvpn3plugintest.cpp
    TEST_NAME openvpn3plugintest
    NAME_PREFIX openvpn3-
    GUI
    LINK_LIBRARIES Qt::Test Qt::Widgets KF6::CoreAddons plasmanetworkmanagement_openvpn3_core
)
add_dependencies(openvpn3plugintest plasmanetworkmanagement_openvpn3ui)
set_tests_properties(openvpn3-openvpn3plugintest PROPERTIES
    ENVIRONMENT_MODIFICATION QT_PLUGIN_PATH=path_list_prepend:${CMAKE_BINARY_DIR}/bin
)

ecm_add_test(
    openvpn3availabilitytest.cpp
    TEST_NAME openvpn3availabilitytest
    NAME_PREFIX openvpn3-
    GUI
    LINK_LIBRARIES Qt::Test Qt::Widgets Qt::DBus KF6::CoreAddons plasmanetworkmanagement_openvpn3_core
)
add_dependencies(openvpn3availabilitytest plasmanetworkmanagement_openvpn3ui)
set_tests_properties(openvpn3-openvpn3availabilitytest PROPERTIES
    ENVIRONMENT_MODIFICATION QT_PLUGIN_PATH=path_list_prepend:${CMAKE_BINARY_DIR}/bin
)

# Not a test of this module: a test of the host it is installed into. It
# subclasses the installed plasma-nm's own connection editor, so what it
# reports is what that plasma-nm does rather than what its source appears to
# say. Each assertion pins a limitation documented in docs/limitations.md.
ecm_add_test(
    openvpn3hostcompattest.cpp
    TEST_NAME openvpn3hostcompattest
    NAME_PREFIX openvpn3-
    GUI
    LINK_LIBRARIES Qt::Test Qt::Widgets KF6::CoreAddons plasmanetworkmanagement_openvpn3_core
)
add_dependencies(openvpn3hostcompattest plasmanetworkmanagement_openvpn3ui)
set_tests_properties(openvpn3-openvpn3hostcompattest PROPERTIES
    ENVIRONMENT_MODIFICATION QT_PLUGIN_PATH=path_list_prepend:${CMAKE_BINARY_DIR}/bin
)
