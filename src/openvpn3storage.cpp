/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "openvpn3storage.h"

#include "nm-openvpn3-service.h"

#include <QByteArray>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QStringDecoder>

#include <qt6keychain/keychain.h>

namespace
{

std::optional<bool> s_secretServiceAvailability;

const QString flagsKey()
{
    return QLatin1String(NM_OPENVPN3_KEY_PROFILE "-flags");
}

/** Decodes a stored profile, or returns a null string if it is not one. */
QString decodeProfile(const QString &stored)
{
    const auto decoded = QByteArray::fromBase64Encoding(stored.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        return QString();
    }
    // Profiles are text; anything else means the value is not a profile.
    auto toUtf16 = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = toUtf16(*decoded);
    return toUtf16.hasError() ? QString() : text;
}

/** The storage marker, or an empty string for the legacy layout. */
QString storageMarker(const NMStringMap &data)
{
    return data.value(QLatin1String(NM_OPENVPN3_KEY_PROFILE_STORAGE));
}

bool isUnsupportedLayout(const NMStringMap &data)
{
    const QString marker = storageMarker(data);
    return !marker.isEmpty() && marker != QLatin1String(NM_OPENVPN3_PROFILE_STORAGE_SECRET);
}

/** The stored (still base64) profile from whichever layout is in use. */
QString storedProfile(const NMStringMap &data, const NMStringMap &secrets)
{
    if (Openvpn3Storage::isSecretMode(data)) {
        // Deliberately no fallback to the data item: in secret mode a public
        // copy is left over from an earlier layout and is stale by definition.
        return secrets.value(QLatin1String(NM_OPENVPN3_KEY_PROFILE));
    }
    return data.value(QLatin1String(NM_OPENVPN3_KEY_PROFILE));
}

}

bool Openvpn3Storage::isSecretMode(const NMStringMap &data)
{
    return storageMarker(data) == QLatin1String(NM_OPENVPN3_PROFILE_STORAGE_SECRET);
}

Openvpn3Storage::Availability Openvpn3Storage::availability(const NMStringMap &data, const NMStringMap &secrets)
{
    if (isUnsupportedLayout(data)) {
        return Availability::Unsupported;
    }
    if (!profileFlags(data)) {
        // A profile marked as never stored is a broken connection, whether or
        // not we happen to be holding the secret at this moment.
        return Availability::UnusableFlags;
    }
    const QString stored = storedProfile(data, secrets);
    if (stored.isEmpty()) {
        return isSecretMode(data) ? Availability::Locked : Availability::Absent;
    }
    return decodeProfile(stored).isNull() ? Availability::Corrupt : Availability::Available;
}

QString Openvpn3Storage::readProfile(const NMStringMap &data, const NMStringMap &secrets)
{
    if (availability(data, secrets) != Availability::Available) {
        return QString();
    }
    return decodeProfile(storedProfile(data, secrets));
}

std::optional<NetworkManager::Setting::SecretFlags> Openvpn3Storage::profileFlags(const NMStringMap &data)
{
    if (isUnsupportedLayout(data)) {
        return std::nullopt;
    }
    if (!isSecretMode(data)) {
        return NetworkManager::Setting::None;
    }
    const QString value = data.value(flagsKey());
    if (value.isEmpty()) {
        // NetworkManager reads a secret without flags as None, system-owned.
        // Guessing AgentOwned would move an unattended connection's profile
        // into the user's wallet the next time anything saved it.
        return NetworkManager::Setting::None;
    }
    bool ok = false;
    const int flags = value.toInt(&ok);
    if (!ok || (flags != NetworkManager::Setting::None && flags != NetworkManager::Setting::AgentOwned)) {
        return std::nullopt;
    }
    return static_cast<NetworkManager::Setting::SecretFlags>(flags);
}

bool Openvpn3Storage::recordsProfileFlags(const NMStringMap &data)
{
    return isSecretMode(data) && data.contains(flagsKey());
}

bool Openvpn3Storage::hostRequestsSecrets(const NMStringMap &data)
{
    for (auto it = data.cbegin(); it != data.cend(); ++it) {
        if (!it.key().endsWith(QLatin1String("-flags"))) {
            continue;
        }
        // Deliberately not checked for being a number: the host does not
        // check either, and QString::toInt() gives 0 -- None -- for anything
        // it cannot read, so the host asks. Mirroring that is the point.
        const auto flags = static_cast<NetworkManager::Setting::SecretFlagType>(it.value().toInt());
        if (flags == NetworkManager::Setting::None || flags == NetworkManager::Setting::AgentOwned) {
            return true;
        }
    }
    return false;
}

void Openvpn3Storage::writeLegacyProfile(NMStringMap &data, NMStringMap &secrets, const QString &profile)
{
    secrets.remove(QLatin1String(NM_OPENVPN3_KEY_PROFILE));
    data.remove(QLatin1String(NM_OPENVPN3_KEY_PROFILE_STORAGE));
    data.remove(flagsKey());
    data.insert(QLatin1String(NM_OPENVPN3_KEY_PROFILE), QString::fromLatin1(profile.toUtf8().toBase64()));
}

bool Openvpn3Storage::writeSecretProfile(NMStringMap &data, NMStringMap &secrets, const QString &profile, NetworkManager::Setting::SecretFlags flags)
{
    if (flags != NetworkManager::Setting::AgentOwned && flags != NetworkManager::Setting::None) {
        return false;
    }
    // No duplicate public copy: it would outlive the secret and be read stale.
    data.remove(QLatin1String(NM_OPENVPN3_KEY_PROFILE));
    data.insert(QLatin1String(NM_OPENVPN3_KEY_PROFILE_STORAGE), QLatin1String(NM_OPENVPN3_PROFILE_STORAGE_SECRET));
    data.insert(flagsKey(), QString::number(static_cast<int>(flags)));
    secrets.insert(QLatin1String(NM_OPENVPN3_KEY_PROFILE), QString::fromLatin1(profile.toUtf8().toBase64()));
    return true;
}

bool Openvpn3Storage::secretServiceIsAvailable()
{
    if (s_secretServiceAvailability) {
        return *s_secretServiceAvailability;
    }
    // QtKeychain's libsecret capability check only tests whether the library
    // can be loaded. It can return true with no bus or provider at all.
    const auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) {
        return false;
    }
    const QDBusReply<QStringList> registered = bus.interface()->registeredServiceNames();
    const QDBusReply<QStringList> activatable = bus.interface()->call(QStringLiteral("ListActivatableNames"));
    if (!registered.isValid() || !activatable.isValid()) {
        return false;
    }
    // Do not open/unlock a wallet from validation. A registered provider or
    // one D-Bus can activate is a prerequisite, not a guarantee that the
    // eventual write succeeds; the secret agent reports write failures.
    for (const QString &provider : {QStringLiteral("org.freedesktop.secrets"), QStringLiteral("org.kde.kwalletd6"),
                                    QStringLiteral("org.kde.kwalletd5"), QStringLiteral("org.kde.kwalletd")}) {
        if (registered.value().contains(provider) || activatable.value().contains(provider)) {
            return QKeychain::isAvailable();
        }
    }
    return false;
}

void Openvpn3Storage::setSecretServiceAvailability(std::optional<bool> available)
{
    s_secretServiceAvailability = available;
}

void Openvpn3Storage::clearProfile(NMStringMap &data, NMStringMap &secrets)
{
    data.remove(QLatin1String(NM_OPENVPN3_KEY_PROFILE));
    data.remove(QLatin1String(NM_OPENVPN3_KEY_PROFILE_STORAGE));
    data.remove(flagsKey());
    secrets.remove(QLatin1String(NM_OPENVPN3_KEY_PROFILE));
}
