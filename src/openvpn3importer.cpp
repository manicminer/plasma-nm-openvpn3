/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "openvpn3importer.h"

#include "nm-openvpn3-service.h"
#include "openvpn3profile.h"
#include "openvpn3storage.h"

#include <QDir>
#include <QFile>
#include <QStringDecoder>
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

/**
 * Why the backend's importer must not be given @p fileName, or nothing.
 *
 * It matches block tags by the literal stripped line where openvpn3 lexes
 * it, so for a quoted or commented opening tag, or an indented closing one,
 * it would read what openvpn3 keeps as a scope or a payload as directives --
 * inline the files and lift the credentials in it. That is refused rather
 * than imported with the scope silently taken apart.
 */
QString importerDisagreement(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString(); // the importer reports that itself
    }
    auto toUtf16 = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = toUtf16(file.readAll());
    if (toUtf16.hasError()) {
        return QString(); // so does it this
    }
    const QString block = Openvpn3Profile::fromText(text).importerTagDisagreement();
    if (block.isEmpty()) {
        return QString();
    }
    return i18n("The <interface>%1</interface> block in this profile is written in a way the OpenVPN 3 backend's importer reads differently "
                "from openvpn3 -- a quoted tag, a comment after the opening tag, or an indented closing tag -- so importing it could take "
                "files or credentials out of it. Write each tag on a line of its own, then import again.",
                block);
}

NMConnection *loadConnection(const QString &fileName, QString *errorMessage)
{
    const QString disagreement = importerDisagreement(fileName);
    if (!disagreement.isEmpty()) {
        *errorMessage = disagreement;
        return nullptr;
    }
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
    // The backend (0.2.0 and later) imports the profile as a system-owned
    // secret already; an older one put it in a public data item. Either way
    // it ends up the secret, with the flags asked for and nothing else
    // marking it: the flags are the marker.
    if (const char *legacy = nm_setting_vpn_get_data_item(s_vpn, NM_OPENVPN3_KEY_PROFILE)) {
        // The value belongs to the setting's table, which the removal frees.
        gchar *stored = g_strdup(legacy);
        nm_setting_vpn_remove_data_item(s_vpn, NM_OPENVPN3_KEY_PROFILE);
        if (!nm_setting_vpn_get_secret(s_vpn, NM_OPENVPN3_KEY_PROFILE)) {
            nm_setting_vpn_add_secret(s_vpn, NM_OPENVPN3_KEY_PROFILE, stored);
        }
        g_free(stored);
    }
    nm_setting_vpn_remove_data_item(s_vpn, NM_OPENVPN3_KEY_PROFILE_STORAGE);
    if (!nm_setting_vpn_get_secret(s_vpn, NM_OPENVPN3_KEY_PROFILE)) {
        return;
    }
    nm_setting_set_secret_flags(NM_SETTING(s_vpn), NM_OPENVPN3_KEY_PROFILE, nmFlags, nullptr);
    if (!nm_setting_vpn_get_data_item(s_vpn, NM_OPENVPN3_KEY_REMOTE)) {
        const NMStringMap data{{QLatin1String(NM_OPENVPN3_KEY_PROFILE_FLAGS), QString::number(static_cast<int>(flags))}};
        const NMStringMap secrets{{QLatin1String(NM_OPENVPN3_KEY_PROFILE), QString::fromUtf8(nm_setting_vpn_get_secret(s_vpn, NM_OPENVPN3_KEY_PROFILE))}};
        const QString remote = Openvpn3Storage::displayRemote(Openvpn3Storage::readProfile(data, secrets));
        if (!remote.isEmpty()) {
            nm_setting_vpn_add_data_item(s_vpn, NM_OPENVPN3_KEY_REMOTE, remote.toUtf8().constData());
        }
    }
}

/** Every data item and secret of @p s_vpn, as the maps the rest of this
 * module reads a connection from. */
void vpnMaps(NMSettingVpn *s_vpn, NMStringMap &data, NMStringMap &secrets)
{
    nm_setting_vpn_foreach_data_item(
        s_vpn,
        [](const char *key, const char *value, gpointer user) {
            static_cast<NMStringMap *>(user)->insert(QString::fromUtf8(key), QString::fromUtf8(value));
        },
        &data);
    nm_setting_vpn_foreach_secret(
        s_vpn,
        [](const char *key, const char *value, gpointer user) {
            static_cast<NMStringMap *>(user)->insert(QString::fromUtf8(key), QString::fromUtf8(value));
        },
        &secrets);
}

}

Openvpn3Import Openvpn3ImporterPrivate::fromConnection(NMConnection *connection)
{
    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    if (!s_vpn) {
        return failure(i18n("The imported connection has no VPN settings"));
    }

    // Wherever the backend put it: a system-owned secret from 0.2.0 on, a
    // public data item before. The storage rules that read a saved
    // connection read this one too, so a stale data copy next to a secret is
    // ignored here exactly as it is there.
    NMStringMap data;
    NMStringMap secrets;
    vpnMaps(s_vpn, data, secrets);
    const QString profile = Openvpn3Storage::readProfile(data, secrets);
    if (profile.isEmpty()) {
        return failure(i18n("The imported connection has no OpenVPN profile"));
    }

    Openvpn3Import result;
    result.m_valid = true;
    result.m_profile = profile;
    result.m_username = dataItem(s_vpn, NM_OPENVPN3_KEY_USERNAME);
    const char *password = nm_setting_vpn_get_secret(s_vpn, NM_OPENVPN3_KEY_PASSWORD);
    result.m_password = password ? QString::fromUtf8(password) : QString();
    // As the backend's service reads it, a <connection> block included: it
    // asks for credentials for an auth-user-pass anywhere outside a payload.
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

bool Openvpn3Importer::backendIsInstalled(QString *errorMessage)
{
    QString message;
    NMVpnEditorPlugin *plugin = editorPlugin(&message);
    if (errorMessage) {
        *errorMessage = message;
    }
    if (!plugin) {
        return false;
    }
    g_object_unref(plugin);
    return true;
}

Openvpn3Import Openvpn3Importer::resolveReferences(const Openvpn3Profile &profile, const QSet<QString> &leave)
{
    QList<quint64> pending;
    bool bareAuthUserPass = false;
    for (int i = 0; i < profile.count(); ++i) {
        const Openvpn3Entry &entry = profile.at(i);
        if (profile.needsNormalizationAt(i) && !leave.contains(profile.sourceAt(i))) {
            pending.append(entry.id());
        } else if (entry.isDirective() && entry.name == QLatin1String("auth-user-pass")) {
            bareAuthUserPass = true;
        }
    }

    Openvpn3Profile resolved = profile;
    const QString newline = profile.usesCrlf() ? QStringLiteral("\r\n") : QStringLiteral("\n");
    QString username;
    QString password;
    bool needsCertPass = false;

    for (const quint64 id : std::as_const(pending)) {
        const int index = resolved.indexOfId(id);
        const Openvpn3Entry entry = resolved.at(index);

        // The entry on its own, with just enough around it for the backend to
        // accept it as a client profile; what it makes of the entry is what
        // goes back in the entry's place, and the scaffolding is dropped.
        QString snippet = QStringLiteral("client\nremote resolve.invalid\n");
        if (entry.isBlock()) {
            QString body = entry.body;
            if (!body.isEmpty() && !body.endsWith(u'\n')) {
                body += u'\n';
            }
            snippet += u'<' + entry.name + QLatin1String(">\n") + body + QLatin1String("</") + entry.name + QLatin1String(">\n");
        } else {
            const QString path = entry.value();
            if (!QDir::isAbsolutePath(path)) {
                // The backend would read it relative to the temporary
                // directory the snippet goes through and report a path
                // nobody wrote.
                return Openvpn3ImporterPrivate::failure(
                    i18n("<interface>%1</interface> names %2, a relative path. A profile written or edited here has no folder that path could be "
                         "relative to: give the full path, or use Import Profile… on the file the profile came from.",
                         entry.name,
                         path));
            }
            QString line = entry.name;
            for (const QString &argument : entry.arguments) {
                line += u' ' + Openvpn3Profile::quoteArgument(argument);
            }
            snippet += line + u'\n';
        }

        const Openvpn3Import piece = normalize(snippet);
        if (!piece.isValid()) {
            return Openvpn3ImporterPrivate::failure(i18n("<interface>%1</interface>: %2", entry.name, piece.errorMessage()));
        }
        QString text = piece.profile();
        if (newline != QLatin1String("\n")) {
            // A referenced file may have CRLF lines of its own, which the
            // backend copies as they are.
            text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
            text.replace(u'\n', newline);
        }
        QList<Openvpn3Entry> replacement = Openvpn3Profile::fromText(text).entries().mid(2);

        if (entry.name == QLatin1String("auth-user-pass")) {
            // As the backend's take_credentials() does across sources: a later
            // one replaces only what it actually has.
            if (!piece.username().isEmpty()) {
                username = piece.username();
            }
            if (!piece.password().isEmpty()) {
                password = piece.password();
            }
            // What the backend writes once, however many sources the
            // credentials had: a second bare directive is not added.
            if (bareAuthUserPass) {
                replacement.clear();
            }
            bareAuthUserPass = true;
        }
        needsCertPass = needsCertPass || piece.needsCertPass();
        resolved.replaceWith(index, replacement);
    }

    return Openvpn3Import::fromProfileText(resolved.toText(), username, password, needsCertPass || resolved.mayNeedPrivateKeyPassphrase());
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
