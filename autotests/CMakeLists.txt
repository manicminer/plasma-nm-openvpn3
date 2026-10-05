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
