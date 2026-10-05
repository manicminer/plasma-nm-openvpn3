/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "openvpn3importer.h"

#include "nm-openvpn3-service.h"
#include "openvpn3profile.h"
#include "openvpn3storage.h"

#include <QFile>
#include <QTemporaryDir>

#include <KLocalizedString>

#include <libnm/nm-vpn-editor-plugin.h>
#include <libnm/nm-vpn-plugin-info.h>

/** Gives the free functions below access to Openvpn3Import's members. */
class Openvpn3ImporterPrivate
{
public:
    static Openvpn3Import failure(const QString &message)
    {
        Openvpn3Import result;
        result.m_errorMessage = message;
        return result;
    }

    static Openvpn3Import fromConnection(NMConnection *connection);
};

namespace
{

/**
 * The openvpn3 backend's libnm plugin, or nullptr with @p errorMessage set.
 *
 * The editor plugin belongs to its NMVpnPluginInfo, so a reference is taken
 * before the info list is released; the caller unrefs it. Without that the
 * plugin dies with the list it came from and the next call through it reads
 * freed memory.
 */
NMVpnEditorPlugin *editorPlugin(QString *errorMessage)
{
    GError *error = nullptr;
    GSList *plugins = nm_vpn_plugin_info_list_load();
    NMVpnPluginInfo *info = nm_vpn_plugin_info_list_find_by_service(plugins, NM_DBUS_SERVICE_OPENVPN3);
    NMVpnEditorPlugin *plugin = nullptr;

    if (!info) {
        *errorMessage = i18n("NetworkManager is missing support for OpenVPN 3");
    } else if ((plugin = nm_vpn_plugin_info_load_editor_plugin(info, &error))) {
        g_object_ref(plugin);
    } else {
        *errorMessage = error ? QString::fromUtf8(error->message) : i18n("Could not load the OpenVPN 3 plugin");
        g_clear_error(&error);
    }

    g_slist_free_full(plugins, g_object_unref);
    return plugin;
}

NMConnection *loadConnection(const QString &fileName, QString *errorMessage)
{
    NMVpnEditorPlugin *plugin = editorPlugin(errorMessage);
    if (!plugin) {
        return nullptr;
    }

    GError *error = nullptr;
    NMConnection *connection = nm_vpn_editor_plugin_import(plugin, fileName.toUtf8().constData(), &error);
    g_object_unref(plugin);
    if (!connection) {
        *errorMessage = error ? QString::fromUtf8(error->message) : i18n("Could not read the OpenVPN profile");
        g_clear_error(&error);
        return nullptr;
    }
    return connection;
}

QString dataItem(NMSettingVpn *s_vpn, const char *key)
{
    const char *value = nm_setting_vpn_get_data_item(s_vpn, key);
    return value ? QString::fromUtf8(value) : QString();
}

/** Moves the profile of @p connection from a public data item to a secret,
 * and the credentials it was imported with to the same storage. */
void migrateToSecretStorage(NMConnection *connection, NetworkManager::Setting::SecretFlags flags)
{
    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    if (!s_vpn) {
        return;
    }
    const auto nmFlags = static_cast<NMSettingSecretFlags>(static_cast<int>(flags));
    // A password the profile carried is left system-owned by the backend,
    // which cannot know what the client around it wants. Here it does: the
    // same place the profile is going. NetworkManager keeps a VPN secret's
    // flags in the data item of that name, which is where the backend put
    // them and the only place they are.
    for (const char *key : {NM_OPENVPN3_KEY_PASSWORD, NM_OPENVPN3_KEY_CERT_PASS}) {
        const QString flagsKey = QLatin1String(key) + QLatin1String("-flags");
        const QString stored = dataItem(s_vpn, flagsKey.toUtf8().constData());
        if (stored.isEmpty()) {
            continue; // the profile does not need this secret at all
        }
        bool ok = false;
        const int current = stored.toInt(&ok);
        // NotSaved is a decision about a one-time code, not a default to move.
        if (ok && (current == NM_SETTING_SECRET_FLAG_NONE || current == NM_SETTING_SECRET_FLAG_AGENT_OWNED)) {
            nm_setting_set_secret_flags(NM_SETTING(s_vpn), key, nmFlags, nullptr);
        }
    }
    // The value belongs to the setting's table, which the removal frees.
    gchar *stored = g_strdup(nm_setting_vpn_get_data_item(s_vpn, NM_OPENVPN3_KEY_PROFILE));
    if (!stored) {
        return;
    }
    nm_setting_vpn_remove_data_item(s_vpn, NM_OPENVPN3_KEY_PROFILE);
    nm_setting_vpn_add_data_item(s_vpn, NM_OPENVPN3_KEY_PROFILE_STORAGE, NM_OPENVPN3_PROFILE_STORAGE_SECRET);
    nm_setting_set_secret_flags(NM_SETTING(s_vpn), NM_OPENVPN3_KEY_PROFILE, static_cast<NMSettingSecretFlags>(static_cast<int>(flags)), nullptr);
    nm_setting_vpn_add_secret(s_vpn, NM_OPENVPN3_KEY_PROFILE, stored);
    g_free(stored);
}

}

Openvpn3Import Openvpn3ImporterPrivate::fromConnection(NMConnection *connection)
{
    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    if (!s_vpn) {
        return failure(i18n("The imported connection has no VPN settings"));
    }

    // The backend's importer always produces the legacy public profile, so
    // that clients which do not know about secret storage keep working; it
    // is Plasma that moves it before the connection reaches NetworkManager.
    const NMStringMap data{{QLatin1String(NM_OPENVPN3_KEY_PROFILE), dataItem(s_vpn, NM_OPENVPN3_KEY_PROFILE)}};
    const QString profile = Openvpn3Storage::readProfile(data, {});
    if (profile.isEmpty()) {
        return failure(i18n("The imported connection has no OpenVPN profile"));
    }

    Openvpn3Import result;
    result.m_valid = true;
    result.m_profile = profile;
    result.m_username = dataItem(s_vpn, NM_OPENVPN3_KEY_USERNAME);
    const char *password = nm_setting_vpn_get_secret(s_vpn, NM_OPENVPN3_KEY_PASSWORD);
    result.m_password = password ? QString::fromUtf8(password) : QString();
    result.m_needsUserPass = Openvpn3Profile::fromText(profile).containsOption(QStringLiteral("auth-user-pass"));
    // The backend flags the passphrase it decided is needed.
    result.m_needsCertPass = !dataItem(s_vpn, NM_OPENVPN3_KEY_CERT_PASS "-flags").isEmpty();
    const char *id = nm_connection_get_id(connection);
    result.m_suggestedId = id ? QString::fromUtf8(id) : QString();
    return result;
}

Openvpn3Import Openvpn3Import::fromProfileText(const QString &profile, const QString &username, const QString &password, bool needsCertPass)
{
    Openvpn3Import result;
    result.m_valid = !profile.isEmpty();
    result.m_profile = profile;
    result.m_username = username;
    result.m_password = password;
    result.m_needsUserPass = Openvpn3Profile::fromText(profile).containsOption(QStringLiteral("auth-user-pass"));
    result.m_needsCertPass = needsCertPass;
    return result;
}

Openvpn3Import Openvpn3Importer::fromFile(const QString &fileName)
{
    QString errorMessage;
    NMConnection *connection = loadConnection(fileName, &errorMessage);
    if (!connection) {
        return Openvpn3ImporterPrivate::failure(errorMessage);
    }

    const Openvpn3Import result = Openvpn3ImporterPrivate::fromConnection(connection);
    g_object_unref(connection);
    return result;
}

Openvpn3Import Openvpn3Importer::normalize(const QString &profileText)
{
    // 0700, and gone again when this scope ends.
    QTemporaryDir dir;
    if (!dir.isValid()) {
        return Openvpn3ImporterPrivate::failure(i18n("Could not create a temporary directory to work in: %1", dir.errorString()));
    }
    const QString path = dir.filePath(QStringLiteral("profile.ovpn"));

    QFile file(path);
    if (!file.open(QIODevice::NewOnly | QIODevice::WriteOnly)) {
        return Openvpn3ImporterPrivate::failure(i18n("Could not write the profile out to be checked: %1", file.errorString()));
    }
    // Before a byte of it is written: the profile may carry a private key.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray encoded = profileText.toUtf8();
    const qint64 written = file.write(encoded);
    // A short write has to be refused rather than imported from: the prefix
    // that got through is a profile the backend reads quite happily, so a full
    // filesystem or a quota would otherwise save a connection with the rest of
    // its configuration silently missing. flush() is where a buffered write
    // finds out, so it is asked before close(), which cannot report.
    const bool complete = written == encoded.size() && file.flush();
    const QString writeError = file.errorString();
    file.close();

    Openvpn3Import result;
    if (complete) {
        result = fromFile(path);
    } else {
        result = Openvpn3ImporterPrivate::failure(i18n("Could not write the whole profile out to be checked: %1", writeError));
    }
    // The directory is removed either way; overwrite first, so that what was
    // in the file does not outlive it in a block nothing reused yet.
    shredInPlace(path, qMax<qint64>(written, 0));
    return result;
}

bool Openvpn3Importer::shredInPlace(const QString &path, qint64 bytes)
{
    if (bytes <= 0) {
        return true;
    }
    QFile file(path);
    // ReadWrite rather than WriteOnly: WriteOnly truncates, which releases
    // the blocks the contents are in before a zero of the replacement has
    // been written, and so overwrites nothing at all. ExistingOnly because
    // ReadWrite would otherwise create the file -- there would be nothing to
    // overwrite, and a path that is already gone would leave one behind.
    if (!file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) {
        return false;
    }
    if (!file.seek(0)) {
        return false;
    }
    const QByteArray zeros(bytes, '\0');
    return file.write(zeros) == zeros.size() && file.flush();
}

Openvpn3Policy Openvpn3Importer::policyOf(const NMStringMap &data)
{
    const auto flagsOr = [&data](const char *key, NetworkManager::Setting::SecretFlags fallback) {
        const QString stored = data.value(QLatin1String(key) + QLatin1String("-flags"));
        bool ok = false;
        const int value = stored.toInt(&ok);
        return ok ? static_cast<NetworkManager::Setting::SecretFlags>(value) : fallback;
    };

    Openvpn3Policy policy;
    if (Openvpn3Storage::isSecretMode(data)) {
        // A connection whose stored flags are nonsense gets the default; the
        // editor refuses to save such a connection before it gets this far.
        policy.profileFlags = Openvpn3Storage::profileFlags(data).value_or(NetworkManager::Setting::AgentOwned);
    }
    policy.passwordFlags = flagsOr(NM_OPENVPN3_KEY_PASSWORD, NetworkManager::Setting::AgentOwned);
    policy.certPassFlags = flagsOr(NM_OPENVPN3_KEY_CERT_PASS, NetworkManager::Setting::AgentOwned);
    return policy;
}

bool Openvpn3Importer::apply(const Openvpn3Import &import, NMStringMap &data, NMStringMap &secrets, const Openvpn3Policy &policy)
{
    if (!import.isValid()) {
        return false;
    }

    NMStringMap newData;
    NMStringMap newSecrets;

    if (!Openvpn3Storage::writeSecretProfile(newData, newSecrets, import.profile(), policy.profileFlags)) {
        // Flags no profile could be stored with; refusing is the only honest
        // answer, and the maps are still exactly as they were.
        return false;
    }

    if (import.needsUserPass()) {
        if (!import.username().isEmpty()) {
            newData.insert(QLatin1String(NM_OPENVPN3_KEY_USERNAME), import.username());
        }
        newData.insert(QLatin1String(NM_OPENVPN3_KEY_PASSWORD "-flags"), QString::number(static_cast<int>(policy.passwordFlags)));
        const bool storable = !policy.passwordFlags.testFlag(NetworkManager::Setting::NotSaved)
            && !policy.passwordFlags.testFlag(NetworkManager::Setting::NotRequired);
        if (!import.password().isEmpty() && storable) {
            newSecrets.insert(QLatin1String(NM_OPENVPN3_KEY_PASSWORD), import.password());
        }
        // One-time codes are asked for on every connect and never stored.
        newData.insert(QLatin1String(NM_OPENVPN3_KEY_CHALLENGE "-flags"), QString::number(NetworkManager::Setting::NotSaved));
    }
    if (import.needsCertPass()) {
        newData.insert(QLatin1String(NM_OPENVPN3_KEY_CERT_PASS "-flags"), QString::number(static_cast<int>(policy.certPassFlags)));
    }

    data = newData;
    secrets = newSecrets;
    return true;
}

bool Openvpn3Importer::apply(const Openvpn3Import &import, NMStringMap &data, NMStringMap &secrets)
{
    return apply(import, data, secrets, policyOf(data));
}

NMConnection *Openvpn3Importer::importConnection(const QString &fileName, NetworkManager::Setting::SecretFlags profileFlags, QString *errorMessage)
{
    if ((profileFlags != NetworkManager::Setting::AgentOwned && profileFlags != NetworkManager::Setting::None)
        || (profileFlags == NetworkManager::Setting::AgentOwned && !Openvpn3Storage::secretServiceIsAvailable())) {
        if (errorMessage) {
            *errorMessage = i18n("No password wallet is available to store this profile. Set up the wallet, or create an OpenVPN 3 connection "
                                 "and explicitly choose storage for all users before importing from its editor.");
        }
        return nullptr;
    }
    QString message;
    NMConnection *connection = loadConnection(fileName, &message);
    if (!connection) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return nullptr;
    }
    migrateToSecretStorage(connection, profileFlags);
    return connection;
}
