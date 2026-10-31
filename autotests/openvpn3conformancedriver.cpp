/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

/**
 * Not a test: the driver through which the backend's front-end conformance
 * suite (network-manager-openvpn3, docs/frontends.md, scripts/conformance)
 * works this editor.
 *
 * Every command goes through the widgets a user would use -- the editor page's
 * own Import Profile path, its fields and its storage choice, and the secrets
 * prompt -- and hands back the VPN setting they produce, as JSON.
 *
 * The editor's storage policy is not the bare core's, deliberately, and the
 * driver does not hide that: a new profile goes to the wallet unless
 * OPENVPN3_DRIVER_STORAGE=system says the user chose storage for all users,
 * and a connection that keeps its profile in the old public layout keeps it
 * there when saved unchanged. docs/verification.md lists what that means for
 * the suite's results.
 *
 * There is no wallet in a conformance run, so the driver tells the editor one
 * is available; whether a write to it succeeds is the secret agent's business
 * and is not what the suite checks.
 */

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QTextStream>

#include <NetworkManagerQt/VpnSetting>

#include "nm-openvpn3-service.h"
#include "openvpn3auth.h"
#include "openvpn3storage.h"
#include "openvpn3widget.h"
#include "passwordfield.h"

namespace
{
int fail(const QString &message)
{
    QTextStream(stderr) << message << Qt::endl;
    return 1;
}

std::optional<QJsonObject> readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(QJsonDocument(object).toJson()) > 0;
}

NMStringMap toMap(const QJsonObject &object)
{
    NMStringMap map;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        map.insert(it.key(), it.value().toString());
    }
    return map;
}

QJsonObject toJson(const NMStringMap &map)
{
    QJsonObject object;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        object.insert(it.key(), it.value());
    }
    return object;
}

/** A VPN setting the way the connection editor hands one over. */
NetworkManager::VpnSetting::Ptr settingOf(const QJsonObject &connection)
{
    const QJsonObject vpn = connection.value(QStringLiteral("vpn")).toObject();
    NetworkManager::VpnSetting source;
    source.setServiceType(vpn.value(QStringLiteral("service-type")).toString(QLatin1String(NM_DBUS_SERVICE_OPENVPN3)));
    source.setData(toMap(vpn.value(QStringLiteral("data")).toObject()));
    const NMStringMap secrets = toMap(vpn.value(QStringLiteral("secrets")).toObject());
    source.setSecrets(secrets);
    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->fromMap(source.toMap());
    setting->setSecrets(secrets);
    return setting;
}

QJsonObject connectionJson(const QString &id, const QVariantMap &vpnMap)
{
    QJsonObject vpn;
    vpn.insert(QStringLiteral("service-type"), vpnMap.value(QStringLiteral("service-type")).toString());
    vpn.insert(QStringLiteral("data"), toJson(qdbus_cast<NMStringMap>(vpnMap.value(QStringLiteral("data")))));
    vpn.insert(QStringLiteral("secrets"), toJson(qdbus_cast<NMStringMap>(vpnMap.value(QStringLiteral("secrets")))));
    QJsonObject connection;
    connection.insert(QStringLiteral("id"), id);
    connection.insert(QStringLiteral("vpn"), vpn);
    return connection;
}

PasswordField::PasswordOption optionFor(const QString &storage)
{
    if (storage == QLatin1String("system")) {
        return PasswordField::StoreForAllUsers;
    }
    if (storage == QLatin1String("ask")) {
        return PasswordField::AlwaysAsk;
    }
    if (storage == QLatin1String("none")) {
        return PasswordField::NotRequired;
    }
    return PasswordField::StoreForUser;
}

int capabilities()
{
    QJsonObject caps;
    caps.insert(QStringLiteral("name"), QStringLiteral("plasma-nm-openvpn3"));
    caps.insert(QStringLiteral("secret-storage"), QJsonArray{QStringLiteral("system"), QStringLiteral("agent"), QStringLiteral("ask"), QStringLiteral("none")});
    caps.insert(QStringLiteral("agent-profile"), true);
    caps.insert(QStringLiteral("secrets"), true);
    QTextStream(stdout) << QJsonDocument(caps).toJson(QJsonDocument::Compact) << Qt::endl;
    return 0;
}

int load(const QString &profile, const QString &out)
{
    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    OpenVpn3SettingWidget widget(setting);
    // The storage choice comes before the import, as on the page.
    const QString storage = qEnvironmentVariable("OPENVPN3_DRIVER_STORAGE", QStringLiteral("agent"));
    auto combo = widget.findChild<QComboBox *>(QStringLiteral("openvpn3_storage"));
    combo->setCurrentIndex(combo->findData(storage == QLatin1String("system") ? NetworkManager::Setting::None : NetworkManager::Setting::AgentOwned));

    QString error;
    if (!widget.importProfile(profile, &error)) {
        return fail(QStringLiteral("import failed: ") + error);
    }
    if (!widget.isValid()) {
        return fail(QStringLiteral("not saveable: ") + widget.blockingProblem());
    }
    return writeJson(out, connectionJson(QFileInfo(profile).completeBaseName(), widget.setting())) ? 0 : fail(QStringLiteral("cannot write ") + out);
}

int edit(const QString &in, const QString &changesPath, const QString &out)
{
    const auto connection = readJson(in);
    const auto changes = readJson(changesPath);
    if (!connection || !changes) {
        return fail(QStringLiteral("cannot read the input"));
    }
    OpenVpn3SettingWidget widget(settingOf(*connection));
    if (changes->contains(QStringLiteral("username"))) {
        widget.findChild<QLineEdit *>(QStringLiteral("openvpn3_username"))->setText(changes->value(QStringLiteral("username")).toString());
    }
    for (const auto &[key, name] : {std::pair{QStringLiteral("password"), QStringLiteral("openvpn3_password")},
                                    std::pair{QStringLiteral("cert-pass"), QStringLiteral("openvpn3_certpass")}}) {
        if (!changes->contains(key)) {
            continue;
        }
        const QJsonObject change = changes->value(key).toObject();
        auto field = widget.findChild<PasswordField *>(name);
        if (change.contains(QStringLiteral("storage"))) {
            field->setPasswordOption(optionFor(change.value(QStringLiteral("storage")).toString()));
        }
        if (change.contains(QStringLiteral("value"))) {
            field->setText(change.value(QStringLiteral("value")).toString());
        }
    }
    if (!widget.isValid()) {
        return fail(QStringLiteral("not saveable: ") + widget.blockingProblem());
    }
    return writeJson(out, connectionJson(connection->value(QStringLiteral("id")).toString(), widget.setting())) ? 0
                                                                                                                   : fail(QStringLiteral("cannot write ") + out);
}

int secrets(const QString &in, const QString &requestPath, const QString &out)
{
    const auto connection = readJson(in);
    const auto request = readJson(requestPath);
    if (!connection || !request) {
        return fail(QStringLiteral("cannot read the input"));
    }
    QStringList hints;
    for (const QJsonValue &hint : request->value(QStringLiteral("hints")).toArray()) {
        hints.append(hint.toString());
    }
    const QString message = request->value(QStringLiteral("message")).toString();
    if (!message.isEmpty()) {
        hints.append(QLatin1String(NM_OPENVPN3_HINT_MESSAGE) + message);
    }
    OpenVpn3AuthWidget widget(settingOf(*connection), hints);
    const QJsonObject answers = request->value(QStringLiteral("answers")).toObject();
    for (PasswordField *field : widget.findChildren<PasswordField *>()) {
        const QString key = field->property("nm_secrets_key").toString();
        if (answers.contains(key)) {
            field->setText(answers.value(key).toString());
        }
    }
    const NMStringMap answer = qdbus_cast<NMStringMap>(widget.setting().value(QStringLiteral("secrets")));
    return writeJson(out, toJson(answer)) ? 0 : fail(QStringLiteral("cannot write ") + out);
}
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", qgetenv("QT_QPA_PLATFORM").isEmpty() ? QByteArray("offscreen") : qgetenv("QT_QPA_PLATFORM"));
    QApplication app(argc, argv);
    Openvpn3Storage::setSecretServiceAvailability(true);

    const QStringList args = app.arguments().mid(1);
    const QString command = args.value(0);
    if (command == QLatin1String("capabilities") && args.size() == 1) {
        return capabilities();
    }
    if (command == QLatin1String("load") && args.size() == 3) {
        return load(args.at(1), args.at(2));
    }
    if (command == QLatin1String("edit") && args.size() == 4) {
        return edit(args.at(1), args.at(2), args.at(3));
    }
    if (command == QLatin1String("secrets") && args.size() == 4) {
        return secrets(args.at(1), args.at(2), args.at(3));
    }
    return fail(QStringLiteral("usage: openvpn3conformancedriver capabilities | load <profile> <out> | edit <in> <changes> <out> | secrets <in> <request> <out>"));
}
