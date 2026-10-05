/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef PLASMA_NM_OPENVPN3_STORAGE_H
#define PLASMA_NM_OPENVPN3_STORAGE_H

#include <optional>

#include <QString>

#include <NetworkManagerQt/GenericTypes>
#include <NetworkManagerQt/Setting>

/**
 * Where an openvpn3 connection keeps its profile.
 *
 * An openvpn3 profile is self-contained: certificates, private keys, PKCS#12
 * bundles and shared TLS keys are inlined into it. Keeping that in @c vpn.data
 * means keeping private key material in a NetworkManager connection file, so
 * a connection may instead hand the whole profile to NetworkManager as a
 * secret. The secret agent -- for Plasma the one in kded, which stores
 * agent-owned secrets in KWallet -- then holds it, or NetworkManager owns it
 * itself so that the connection can come up unattended.
 *
 * The two layouts are mutually exclusive:
 *
 * | mode   | @c vpn.data                                             | @c vpn.secrets |
 * | ------ | ------------------------------------------------------- | -------------- |
 * | legacy | @c profile = base64 profile                             | --             |
 * | secret | @c profile-storage = @c secret, @c profile-flags = 1\|0 | @c profile = base64 profile |
 *
 * @c profile-flags is not a Plasma invention: NetworkManager keeps the secret
 * flags of a VPN secret @c x in <tt>vpn.data["x-flags"]</tt>, so it is simply
 * the flags key of the @c profile secret. @c AgentOwned (1) means the wallet,
 * @c None (0) means NetworkManager stores it for unattended activation.
 *
 * Treating the whole profile as one secret rather than a list of sensitive
 * directives protects directives nobody thought of, and there is no taking
 * apart and putting back together that could lose something.
 *
 * Invariants this module enforces:
 *
 * - In secret mode @c vpn.data["profile"] is removed and never read. A stale
 *   public copy must never stand in for a locked secret, and a backend that
 *   predates the layout finds no profile and fails closed.
 * - A @c profile-storage value this version does not know is refused outright
 *   rather than falling back to the data item: whatever is in there belongs
 *   to the layout the connection used before.
 * - A profile is never written to @c vpn.data as a fallback for an
 *   unavailable wallet.
 * - @c NotSaved and @c NotRequired are rejected for the profile, on reading
 *   as well as on writing: they describe a profile nobody could reconstruct,
 *   so a connection claiming them is broken rather than system-owned.
 */
namespace Openvpn3Storage
{

enum class Availability {
    Available, //!< the profile is here
    Locked, //!< secret mode, and the secret was not handed to us
    Absent, //!< the connection has no profile at all
    Corrupt, //!< something is stored, but it is not a base64 UTF-8 profile
    Unsupported, //!< the connection names a storage layout this version does not know
    UnusableFlags, //!< the profile is marked with flags that would never store it
};

/** True when the profile of this connection is one of its secrets. */
bool isSecretMode(const NMStringMap &data);

/** Where the profile is, and whether it can be read right now. */
Availability availability(const NMStringMap &data, const NMStringMap &secrets);

/** The profile text, or an empty string when it is not available. */
QString readProfile(const NMStringMap &data, const NMStringMap &secrets);

/**
 * Secret flags of the profile; @c None for a legacy public profile, and for
 * a secret one without flags -- which is what NetworkManager itself reads
 * absent flags as.
 *
 * @return nothing when the connection names a layout this version does not
 * know, or flags that would never store the profile.
 */
std::optional<NetworkManager::Setting::SecretFlags> profileFlags(const NMStringMap &data);

/** Stores @p profile as a public data item and clears the secret layout. */
void writeLegacyProfile(NMStringMap &data, NMStringMap &secrets, const QString &profile);

/**
 * Stores @p profile as the connection's @c profile secret with @p flags and
 * removes the public copy.
 *
 * @p flags must be @c AgentOwned (wallet) or @c None (system). Anything else
 * is a programming error and leaves the maps untouched. A new profile goes
 * to the wallet unless the caller says otherwise.
 * @return false if @p flags was rejected.
 */
bool writeSecretProfile(NMStringMap &data,
                        NMStringMap &secrets,
                        const QString &profile,
                        NetworkManager::Setting::SecretFlags flags = NetworkManager::Setting::AgentOwned);

/** Drops every trace of a profile from both maps. */
void clearProfile(NMStringMap &data, NMStringMap &secrets);

/**
 * True when a session bus and a registered or activatable wallet provider exist.
 *
 * Plasma's secret agent keeps agent-owned secrets in a secret service, and
 * when there is none it keeps nothing at all -- it does not fall back to a
 * file. A profile saved as an agent-owned secret would then be gone, so the
 * editor has to say so first rather than let it be written.
 *
 * This is only whether storing is possible. Whether a particular write
 * succeeded is the agent's to report, and it does.
 */
bool secretServiceIsAvailable();

/**
 * Makes secretServiceIsAvailable() answer @p available; nothing resets it.
 *
 * Used by component tests to exercise storage policy independently of desktop
 * state. The availability tests separately exercise the real bus probe.
 */
void setSecretServiceAvailability(std::optional<bool> available);

}

#endif // PLASMA_NM_OPENVPN3_STORAGE_H
