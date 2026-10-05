# SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

add_definitions(-DTRANSLATION_DOMAIN=\"plasmanetworkmanagement_openvpn3ui\")

# Everything except the plugin entry point lives in a static library so that
# the autotests can link it without loading the plugin.
add_library(plasmanetworkmanagement_openvpn3_core STATIC)
target_sources(plasmanetworkmanagement_openvpn3_core PRIVATE
    openvpn3profile.cpp
    openvpn3profile.h
    openvpn3storage.cpp
    openvpn3storage.h
    nm-openvpn3-service.h
)
set_property(TARGET plasmanetworkmanagement_openvpn3_core PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(plasmanetworkmanagement_openvpn3_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(plasmanetworkmanagement_openvpn3_core
PUBLIC
    KF6::NetworkManagerQt
    Qt::Core
    Qt::DBus
PRIVATE
    Qt6Keychain::Qt6Keychain
)
