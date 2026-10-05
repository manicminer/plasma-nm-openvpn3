/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef PLASMA_NM_OPENVPN3_IMPORTER_H
#define PLASMA_NM_OPENVPN3_IMPORTER_H

#include <QString>

#include <NetworkManagerQt/GenericTypes>
#include <NetworkManagerQt/Setting>

#include "nm-connection.h"

/**
 * The outcome of importing a .ovpn file.
 *
 * Importing goes through the openvpn3 backend's own libnm plugin rather than
 * a reimplementation here: it is the backend that decides how a profile is
 * normalised -- which file references are inlined, how a PKCS#12 bundle is
 * embedded, which credentials are lifted out of the profile -- and a second
 * opinion on that would only drift.
 */
class Openvpn3Import
{
public:
    bool isValid() const
    {
        return m_valid;
    }
    QString errorMessage() const
    {
        return m_errorMessage;
    }
    /** The normalised, self-contained profile: nothing it refers to is
     * needed on disk any more. */
    QString profile() const
    {
        return m_profile;
    }
    QString username() const
    {
        return m_username;
    }
    /** A password the profile carried, if any. */
    QString password() const
    {
        return m_password;
    }
    /** The profile asks for a username and password. */
    bool needsUserPass() const
    {
        return m_needsUserPass;
    }
    /** The profile has an encrypted key or a PKCS#12 bundle. */
    bool needsCertPass() const
    {
        return m_needsCertPass;
    }
    /** Connection name suggested by the file name. */
    QString suggestedId() const
    {
        return m_suggestedId;
    }

    /** An import result built from profile text that is already normalised. */
    static Openvpn3Import fromProfileText(const QString &profile, const QString &username = QString(), const QString &password = QString(),
                                          bool needsCertPass = false);

private:
    friend class Openvpn3ImporterPrivate;
    bool m_valid = false;
    bool m_needsUserPass = false;
    bool m_needsCertPass = false;
    QString m_errorMessage;
    QString m_profile;
    QString m_username;
    QString m_password;
    QString m_suggestedId;
};

/**
 * Where a connection's secrets are kept, as the user asked for it.
 *
 * None of this comes out of the imported file: a reimport replaces the
 * configuration, not the policy.
 */
struct Openvpn3Policy {
    /** Secret flags for the profile. An imported profile is always one of the
     * connection's secrets: it is new key material, and the old public data
     * item is not somewhere new key material is put. */
    NetworkManager::Setting::SecretFlags profileFlags = NetworkManager::Setting::AgentOwned;
    NetworkManager::Setting::SecretFlags passwordFlags = NetworkManager::Setting::AgentOwned;
    NetworkManager::Setting::SecretFlags certPassFlags = NetworkManager::Setting::AgentOwned;
};

namespace Openvpn3Importer
{

/** Imports @p fileName through the openvpn3 backend's libnm plugin. */
Openvpn3Import fromFile(const QString &fileName);

/**
 * Normalises @p profileText through the same backend importer, for a profile
 * that was written or edited here rather than imported.
 *
 * openvpn3 is handed a profile and nothing else, so one that points at a file
 * or carries credentials inline has to be made self-contained first; see
 * Openvpn3Profile::needsNormalization(). The text goes through a file only
 * its owner can read, inside a directory only its owner can enter, and both
 * are gone before this returns.
 *
 * @return an invalid import with an error message when the profile cannot be
 * normalised -- a file it names cannot be read, say.
 */
Openvpn3Import normalize(const QString &profileText);

/**
 * Overwrites the first @p bytes of @p path with zeros, in place.
 *
 * A profile written out to be normalised can carry a private key, and the
 * file is about to be unlinked, so the bytes are overwritten where they are
 * rather than the file truncated: truncating first would release the blocks
 * with their contents still in them and write the zeros somewhere else.
 *
 * Best effort, and not a secure erase. A copy-on-write or journalling
 * filesystem can still hold the old contents elsewhere, and the file this is
 * used on normally lives on a tmpfs, where it never reached a disk to begin
 * with. It removes the easy case, not every case.
 *
 * @return false when the file could not be opened, written or flushed.
 */
bool shredInPlace(const QString &path, qint64 bytes);

/** The policy @p data already expresses, for a reimport that is not to change
 * it. A connection that never made a choice gets the wallet. */
Openvpn3Policy policyOf(const NMStringMap &data);

/**
 * Replaces the VPN configuration of @p data / @p secrets with @p import,
 * storing its secrets as @p policy says.
 *
 * All or nothing: an import that failed leaves both maps exactly as they
 * were, so a cancelled or broken reimport cannot half-destroy a connection.
 *
 * @return false when @p import is not valid; the maps are then untouched.
 */
bool apply(const Openvpn3Import &import, NMStringMap &data, NMStringMap &secrets, const Openvpn3Policy &policy);

/** As above, keeping the policy @p data already expresses. */
bool apply(const Openvpn3Import &import, NMStringMap &data, NMStringMap &secrets);

/**
 * Imports @p fileName as a complete new NetworkManager connection, with the
 * profile and the credentials that came with it already moved to
 * @p profileFlags storage.
 *
 * The backend's importer leaves a password it found in the profile
 * system-owned, because it has no way of knowing what the client around it
 * wants; a connection imported into Plasma asks for the wallet, and the
 * password it was imported with belongs there too rather than in
 * NetworkManager's own store by accident.
 *
 * The caller owns the returned connection.
 */
NMConnection *importConnection(const QString &fileName, NetworkManager::Setting::SecretFlags profileFlags, QString *errorMessage);

}

#endif // PLASMA_NM_OPENVPN3_IMPORTER_H
