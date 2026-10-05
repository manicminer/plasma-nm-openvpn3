/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef PLASMA_NM_OPENVPN3_H
#define PLASMA_NM_OPENVPN3_H

#include <QVariant>

#include "vpnuiplugin.h"

/**
 * The Plasma editor for org.freedesktop.NetworkManager.openvpn3 connections.
 *
 * Separate from the OpenVPN 2 plugin: the two speak to different services and
 * store their configuration differently, even though they read the same .ovpn
 * files. Importing goes through the openvpn3 backend's own libnm plugin.
 *
 * supportedFileExtensions() is deliberately not overridden, so this plugin
 * offers the host's top-level VPN import nothing at all; see
 * docs/limitations.md. The editor's own Import button is unaffected.
 */
class Q_DECL_EXPORT OpenVpn3UiPlugin : public VpnUiPlugin
{
    Q_OBJECT

public:
    explicit OpenVpn3UiPlugin(QObject *parent = nullptr, const QVariantList & = QVariantList());
    ~OpenVpn3UiPlugin() override;

    SettingWidget *widget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent) override;
    SettingWidget *askUser(const NetworkManager::VpnSetting::Ptr &setting, const QStringList &hints, QWidget *parent) override;

    QString suggestedFileName(const NetworkManager::ConnectionSettings::Ptr &connection) const override;
    ImportResult importConnectionSettings(const QString &fileName) override;
    ExportResult exportConnectionSettings(const NetworkManager::ConnectionSettings::Ptr &connection, const QString &fileName) override;
};

#endif // PLASMA_NM_OPENVPN3_H
