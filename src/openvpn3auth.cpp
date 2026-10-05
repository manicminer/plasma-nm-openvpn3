/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "openvpn3auth.h"

#include <QFormLayout>
#include <QLabel>

#include <KLocalizedString>

#include "nm-openvpn3-service.h"
#include "openvpn3profile.h"
#include "openvpn3storage.h"
#include "passwordfield.h"

namespace
{
/** A prompt label that reads like one, whatever the service sent. */
QString asPrompt(QString text)
{
    if (text.isEmpty()) {
        return text;
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    if (!text.endsWith(QLatin1Char(':'))) {
        text += QLatin1Char(':');
    }
    return text;
}

bool flagIsSet(const NMStringMap &data, const QString &key, NetworkManager::Setting::SecretFlagType flag)
{
    bool ok = false;
    const int value = data.value(key + QLatin1String("-flags")).toInt(&ok);
    return ok && (value & flag);
}
}

class OpenVpn3AuthWidget::Private
{
public:
    NetworkManager::VpnSetting::Ptr setting;
    QFormLayout *layout = nullptr;
};

OpenVpn3AuthWidget::OpenVpn3AuthWidget(const NetworkManager::VpnSetting::Ptr &setting, const QStringList &hints, QWidget *parent)
    : SettingWidget(setting, hints, parent)
    , d(new Private)
{
    d->setting = setting;
    d->layout = new QFormLayout(this);
    setLayout(d->layout);

    readSecrets();

    KAcceleratorManager::manage(this);
}

OpenVpn3AuthWidget::~OpenVpn3AuthWidget()
{
    delete d;
}

void OpenVpn3AuthWidget::addField(const QString &label, const QString &key, const QString &value, bool echo)
{
    auto field = new PasswordField(this);
    field->setPasswordModeEnabled(!echo);
    field->setProperty("nm_secrets_key", key);
    field->setText(value);
    d->layout->addRow(new QLabel(label, this), field);
}

void OpenVpn3AuthWidget::addMessage(const QString &text)
{
    auto label = new QLabel(text, this);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    d->layout->addRow(label);
}

void OpenVpn3AuthWidget::focusFirstEmptyField()
{
    for (int row = 0; row < d->layout->rowCount(); ++row) {
        QLayoutItem *item = d->layout->itemAt(row, QFormLayout::FieldRole);
        auto field = item ? qobject_cast<PasswordField *>(item->widget()) : nullptr;
        if (field && field->text().isEmpty()) {
            field->setFocus(Qt::OtherFocusReason);
            return;
        }
    }
}

void OpenVpn3AuthWidget::readSecrets()
{
    const NMStringMap data = d->setting ? d->setting->data() : NMStringMap();
    const NMStringMap secrets = d->setting ? d->setting->secrets() : NMStringMap();

    QString message;
    bool challengeEcho = false;
    for (const QString &hint : std::as_const(m_hints)) {
        if (hint.startsWith(QLatin1String(NM_OPENVPN3_HINT_MESSAGE))) {
            message = hint.sliced(qstrlen(NM_OPENVPN3_HINT_MESSAGE));
        } else if (hint == QLatin1String(NM_OPENVPN3_HINT_CHALLENGE_ECHO)) {
            challengeEcho = true;
        }
    }

    // A hint is the service asking for one named thing, and it is asking
    // because it already has the profile: the session is running. The profile
    // this request carries has nothing to do with it -- a retry comes with
    // NetworkManager's RequestNew, and the agent does not read the wallet for
    // one, so there will be no profile here however healthy the connection
    // is. Refusing on that would make a wrong password unrecoverable.
    if (m_hints.contains(QLatin1String(NM_OPENVPN3_KEY_CHALLENGE))) {
        // A one-time code: never prefilled, never stored, and echoed when the
        // server said it may be (a token on a screen the user is reading off).
        addField(asPrompt(message.isEmpty() ? i18n("One-time code") : message),
                 QLatin1String(NM_OPENVPN3_KEY_CHALLENGE),
                 QString(),
                 challengeEcho);
        focusFirstEmptyField();
        return;
    }
    if (m_hints.contains(QLatin1String(NM_OPENVPN3_KEY_CERT_PASS))) {
        addField(asPrompt(message.isEmpty() ? i18n("Private key passphrase") : message), QLatin1String(NM_OPENVPN3_KEY_CERT_PASS), QString(), false);
        focusFirstEmptyField();
        return;
    }
    if (m_hints.contains(QLatin1String(NM_OPENVPN3_KEY_PASSWORD))) {
        addField(asPrompt(message.isEmpty() ? i18n("Password") : message),
                 QLatin1String(NM_OPENVPN3_KEY_PASSWORD),
                 secrets.value(QLatin1String(NM_OPENVPN3_KEY_PASSWORD)),
                 false);
        focusFirstEmptyField();
        return;
    }

    // No hints: the service wants the connection's stored secrets, and which
    // of them it needs is written in the profile.
    const Openvpn3Storage::Availability availability = Openvpn3Storage::availability(data, secrets);
    if (availability != Openvpn3Storage::Availability::Available) {
        // Without the profile nothing here knows what the connection needs,
        // and the one thing it certainly needs is the profile. A password box
        // would collect an answer that cannot be the problem or the fix.
        addMessage(unavailableProfileMessage(availability));
        return;
    }

    const Openvpn3Profile profile = Openvpn3Profile::fromText(Openvpn3Storage::readProfile(data, secrets));
    // What openvpn3 reads, which includes the options inside a <connection>
    // block: a profile may ask for a username and password in there.
    const bool wantsPassword = profile.containsOption(QStringLiteral("auth-user-pass"));
    const bool wantsPassphrase = profile.mayNeedPrivateKeyPassphrase();

    if (wantsPassword && !flagIsSet(data, QLatin1String(NM_OPENVPN3_KEY_PASSWORD), NetworkManager::Setting::NotRequired)) {
        addField(i18n("Password:"), QLatin1String(NM_OPENVPN3_KEY_PASSWORD), secrets.value(QLatin1String(NM_OPENVPN3_KEY_PASSWORD)), false);
    }
    if (wantsPassphrase && !flagIsSet(data, QLatin1String(NM_OPENVPN3_KEY_CERT_PASS), NetworkManager::Setting::NotRequired)) {
        addField(i18n("Private key passphrase:"), QLatin1String(NM_OPENVPN3_KEY_CERT_PASS), secrets.value(QLatin1String(NM_OPENVPN3_KEY_CERT_PASS)), false);
    }

    if (d->layout->rowCount() == 0) {
        // The connection needs secrets that are already stored -- the profile
        // itself, typically. Say so rather than present an empty dialog.
        addMessage(i18n("No further details are needed; the stored secrets will be used."));
    }

    focusFirstEmptyField();
}

QString OpenVpn3AuthWidget::unavailableProfileMessage(Openvpn3Storage::Availability availability)
{
    switch (availability) {
    case Openvpn3Storage::Availability::Locked:
        return i18n(
            "The OpenVPN profile of this connection is kept with its secrets and could not be read. "
            "Unlock the password wallet and try again; no password typed here could stand in for it.");
    case Openvpn3Storage::Availability::Absent:
        return i18n("This connection has no OpenVPN profile. Open its settings and import one.");
    case Openvpn3Storage::Availability::Corrupt:
        return i18n("The OpenVPN profile of this connection cannot be read. Open its settings and import one to replace it.");
    case Openvpn3Storage::Availability::Unsupported:
        return i18n(
            "This connection stores its OpenVPN profile in a way this version of Plasma does not know. "
            "Update plasma-nm, or open the connection's settings and import a profile to replace it.");
    case Openvpn3Storage::Availability::UnusableFlags:
        return i18n(
            "This connection says its OpenVPN profile is never stored, which cannot be true of a profile. "
            "Open its settings and import one to repair it.");
    case Openvpn3Storage::Availability::Available:
        break;
    }
    return QString();
}

QVariantMap OpenVpn3AuthWidget::setting() const
{
    // Start from everything the connection already carries. The secret agent
    // replaces the whole vpn setting with what this returns, so a secret left
    // out here is a secret NetworkManager will not have -- and for an
    // openvpn3 connection one of them may be the profile.
    NMStringMap secrets = d->setting ? d->setting->secrets() : NMStringMap();

    secrets.remove(QLatin1String(NM_OPENVPN3_KEY_CHALLENGE));

    for (int row = 0; row < d->layout->rowCount(); ++row) {
        QLayoutItem *item = d->layout->itemAt(row, QFormLayout::FieldRole);
        auto field = item ? qobject_cast<PasswordField *>(item->widget()) : nullptr;
        if (!field) {
            continue;
        }
        const QString key = field->property("nm_secrets_key").toString();
        if (!key.isEmpty() && !field->text().isEmpty()) {
            secrets.insert(key, field->text());
        }
    }

    QVariantMap result;
    result.insert(QStringLiteral("secrets"), QVariant::fromValue<NMStringMap>(secrets));
    return result;
}

#include "moc_openvpn3auth.cpp"
