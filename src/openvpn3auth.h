/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef PLASMA_NM_OPENVPN3_AUTH_H
#define PLASMA_NM_OPENVPN3_AUTH_H

#include <NetworkManagerQt/VpnSetting>

#include "openvpn3storage.h"
#include "settingwidget.h"

class QFormLayout;
class QLabel;

/**
 * The secrets prompt for an openvpn3 connection.
 *
 * NetworkManager asks the agent for the whole @c vpn setting at once and
 * replaces what it has with the answer, so this widget hands back every
 * secret the connection already carries -- including the profile, when the
 * profile is one of them -- alongside whatever the user just typed. Returning
 * only the typed value would take the profile away from the activation that
 * asked for it.
 *
 * Which field to show comes from the hints the service sent: a one-time code,
 * a private key passphrase, or the password. A hinted request is answered
 * whatever state the connection appears to be in -- it is a retry, and
 * NetworkManager's RequestNew means the agent did not read the wallet, so the
 * profile is not here even for a connection that is perfectly healthy.
 *
 * Without hints the connection's own profile decides, and a profile that
 * could not be read is said so rather than answered with a password box: the
 * thing that is missing is the profile, and no password is that.
 */
class OpenVpn3AuthWidget : public SettingWidget
{
    Q_OBJECT

public:
    explicit OpenVpn3AuthWidget(const NetworkManager::VpnSetting::Ptr &setting, const QStringList &hints, QWidget *parent = nullptr);
    ~OpenVpn3AuthWidget() override;

    QVariantMap setting() const override;

private:
    void readSecrets();
    void addField(const QString &label, const QString &key, const QString &value, bool echo);
    //! An explanation where a field would be: a row with nothing to type in.
    void addMessage(const QString &text);
    /** Puts the cursor where the user has to type -- the one-time code, when
     * that is what was asked for. */
    void focusFirstEmptyField();
    //! Why a profile that is not here cannot be made up for by typing.
    static QString unavailableProfileMessage(Openvpn3Storage::Availability availability);

    class Private;
    Private *const d;
};

#endif // PLASMA_NM_OPENVPN3_AUTH_H
