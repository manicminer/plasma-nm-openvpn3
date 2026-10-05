/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "openvpn3.h"

#include <KLocalizedString>
#include <KPluginFactory>

#include "nm-openvpn3-service.h"
#include "openvpn3auth.h"
#include "openvpn3importer.h"
#include "openvpn3widget.h"

K_PLUGIN_CLASS_WITH_JSON(OpenVpn3UiPlugin, "plasmanetworkmanagement_openvpn3ui.json")

OpenVpn3UiPlugin::OpenVpn3UiPlugin(QObject *parent, const QVariantList &)
    : VpnUiPlugin(parent)
{
}

OpenVpn3UiPlugin::~OpenVpn3UiPlugin() = default;

SettingWidget *OpenVpn3UiPlugin::widget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent)
{
    return new OpenVpn3SettingWidget(setting, parent);
}

SettingWidget *OpenVpn3UiPlugin::askUser(const NetworkManager::VpnSetting::Ptr &setting, const QStringList &hints, QWidget *parent)
{
    return new OpenVpn3AuthWidget(setting, hints, parent);
}

QString OpenVpn3UiPlugin::suggestedFileName(const NetworkManager::ConnectionSettings::Ptr &connection) const
{
    return connection->id() + QStringLiteral(".ovpn");
}

// supportedFileExtensions() is deliberately left as VpnUiPlugin's, which
// claims nothing.
//
// A stock plasma-nm's top-level VPN import takes the first installed plugin
// that claims the file's extension, in whatever order the plugins came back
// in. Both OpenVPN plugins read .ovpn, so claiming it here would mean .ovpn
// imports sometimes arriving as OpenVPN 3 connections on a machine where
// nothing was asked and nothing changed except that this module was
// installed -- a change to how an unrelated VPN behaves, made silently.
// Asking which was meant needs a host that offers the choice, which a stock
// one does not, so this plugin stays out of that path altogether and OpenVPN
// 2 keeps every .ovpn and .conf.
//
// Importing one as an OpenVPN 3 connection is still a click away: create the
// connection and use the editor page's own Import button, which calls the
// importer below directly and never goes near this list.
VpnUiPlugin::ImportResult OpenVpn3UiPlugin::importConnectionSettings(const QString &fileName)
{
    QString errorMessage;
    // A new connection keeps its profile in the wallet: it carries the
    // private keys, and there is a logged-in user to ask by definition.
    NMConnection *connection = Openvpn3Importer::importConnection(fileName, NetworkManager::Setting::AgentOwned, &errorMessage);
    if (!connection) {
        return VpnUiPlugin::ImportResult::fail(errorMessage);
    }
    return VpnUiPlugin::ImportResult::pass(connection);
}

VpnUiPlugin::ExportResult OpenVpn3UiPlugin::exportConnectionSettings(const NetworkManager::ConnectionSettings::Ptr &connection, const QString &fileName)
{
    Q_UNUSED(connection)
    Q_UNUSED(fileName)
    // An openvpn3 profile is self-contained: it has the private key and the
    // certificates inlined in it, and when it is kept with the connection's
    // secrets it is kept there precisely so that it never reaches a plain
    // file. Writing one out would undo that without saying so, so there is
    // no export rather than an export that quietly leaks.
    return VpnUiPlugin::ExportResult::fail(
        i18n("An OpenVPN 3 profile contains its private keys, so Plasma does not write it back out to a file."));
}

#include "openvpn3.moc"

#include "moc_openvpn3.cpp"
