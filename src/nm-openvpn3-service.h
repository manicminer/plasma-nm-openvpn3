/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef NM_OPENVPN3_SERVICE_H
#define NM_OPENVPN3_SERVICE_H

#define NM_DBUS_SERVICE_OPENVPN3 "org.freedesktop.NetworkManager.openvpn3"

/* The whole profile, base64-encoded.  Either a public data item (legacy) or
 * one of the connection's secrets; see openvpn3storage.h. */
#define NM_OPENVPN3_KEY_PROFILE "profile"
#define NM_OPENVPN3_KEY_PROFILE_STORAGE "profile-storage"
#define NM_OPENVPN3_PROFILE_STORAGE_SECRET "secret"

#define NM_OPENVPN3_KEY_USERNAME "username"
#define NM_OPENVPN3_KEY_PASSWORD "password"
/* Passphrase of the private key: PKCS#12 bundle or encrypted PEM key. */
#define NM_OPENVPN3_KEY_CERT_PASS "cert-pass"
/* One-time code; never stored. */
#define NM_OPENVPN3_KEY_CHALLENGE "challenge-response"

/* Hints the service adds to a secrets request. */
#define NM_OPENVPN3_HINT_MESSAGE "x-vpn-message:"
#define NM_OPENVPN3_HINT_CHALLENGE_ECHO "x-challenge-echo"

#endif // NM_OPENVPN3_SERVICE_H
