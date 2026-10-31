/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef OPENVPN3_BACKEND_GUARD_H
#define OPENVPN3_BACKEND_GUARD_H

#include <QTest>

#include "openvpn3importer.h"

/**
 * Whether the openvpn3 backend's libnm plugin is installed.
 *
 * Asked of libnm, not of an import: a backend that is there and imports
 * something this module no longer understands is a failure to report, and
 * guarding on a successful import would turn exactly that into a skip.
 */
inline bool openvpn3BackendInstalled()
{
    static const bool installed = Openvpn3Importer::backendIsInstalled();
    return installed;
}

/**
 * Skips a test that needs the real importer where the backend is not
 * installed -- unless OPENVPN3_REQUIRE_BACKEND is set, which the container
 * harness does for an image built with the backend, and then it fails. A
 * run that was meant to exercise the real importer cannot pass by skipping.
 */
#define OPENVPN3_REQUIRE_BACKEND()                                                                                                                             \
    do {                                                                                                                                                       \
        if (!openvpn3BackendInstalled()) {                                                                                                                    \
            if (qEnvironmentVariableIsSet("OPENVPN3_REQUIRE_BACKEND")) {                                                                                      \
                QFAIL("OPENVPN3_REQUIRE_BACKEND is set, but the openvpn3 backend's libnm plugin is not installed");                                           \
            }                                                                                                                                                  \
            QSKIP("the openvpn3 backend's libnm plugin is not installed");                                                                                     \
        }                                                                                                                                                      \
    } while (false)

#endif // OPENVPN3_BACKEND_GUARD_H
