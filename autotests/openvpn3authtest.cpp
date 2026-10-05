/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <QLabel>
#include <QLineEdit>
#include <QTest>

#include <NetworkManagerQt/VpnSetting>

#include "nm-openvpn3-service.h"
#include "openvpn3auth.h"
#include "passwordfield.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
const auto kProfile = QStringLiteral("client\nremote vpn.example.org 1194\nauth-user-pass\n");
const auto kCertProfile = QStringLiteral("client\nremote vpn.example.org 1194\n");

QString encoded(const QString &text)
{
    return QString::fromLatin1(text.toUtf8().toBase64());
}

NetworkManager::VpnSetting::Ptr secretSetting(const QString &profile = kProfile, const NMStringMap &extraSecrets = {}, const NMStringMap &extraData = {})
{
    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    NMStringMap data{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}};
    data.insert(extraData);
    NMStringMap secrets{{u"profile"_s, encoded(profile)}};
    secrets.insert(extraSecrets);
    setting->setData(data);
    setting->setSecrets(secrets);
    return setting;
}

QList<PasswordField *> fields(OpenVpn3AuthWidget *widget)
{
    return widget->findChildren<PasswordField *>();
}

QStringList keys(OpenVpn3AuthWidget *widget)
{
    QStringList result;
    const auto all = fields(widget);
    for (PasswordField *field : all) {
        result.append(field->property("nm_secrets_key").toString());
    }
    return result;
}

QStringList labels(OpenVpn3AuthWidget *widget)
{
    QStringList result;
    const auto all = widget->findChildren<QLabel *>();
    for (QLabel *label : all) {
        result.append(label->text());
    }
    return result;
}

NMStringMap returnedSecrets(OpenVpn3AuthWidget *widget)
{
    return qdbus_cast<NMStringMap>(widget->setting().value(u"secrets"_s));
}
}

/** The secrets prompt: what it asks for, and what it hands back. */
class Openvpn3AuthTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void staleChallengeIsNotReturned()
    {
        for (const QStringList &hints : {QStringList(), QStringList{u"password"_s}, QStringList{u"challenge-response"_s}}) {
            OpenVpn3AuthWidget widget(secretSetting(kProfile, {{u"challenge-response"_s, u"old-otp"_s}}), hints);
            QVERIFY(!returnedSecrets(&widget).contains(u"challenge-response"_s));
        }
    }
    void nestedEncryptedMaterialAsksForPassphrase()
    {
        for (const QString &material : {u"<key>\n-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n</key>\n"_s,
                                        u"<pkcs12>\nAAAA\n</pkcs12>\n"_s}) {
            OpenVpn3AuthWidget widget(secretSetting(u"client\n<connection>\nremote example.org\n"_s + material + u"</connection>\n"_s), {});
            QCOMPARE(keys(&widget), QStringList{u"cert-pass"_s});
        }
    }

    void aChallengeHintAsksForAOneTimeCode();
    void anEchoedChallengeIsStillASecret();
    void aChallengeIsNeverPrefilled();
    void theServiceMessageBecomesTheLabel();
    void aCertPassHintAsksForThePassphrase();
    void withoutHintsTheProfileDecides();
    void aCertificateOnlyProfileAsksForNothing();
    void notRequiredSecretsAreNotAskedFor();
    void storedSecretsAreHandedBackIncludingTheProfile();
    void whatTheUserTypedIsAddedToTheStoredSecrets();
    void nothingIsReturnedOutsideTheSecretsMap();
    void aPlainPrivateKeyDoesNotAskForAPassphrase();
    void anEncryptedPrivateKeyAsksForAPassphrase();

    void aProfileThatCouldNotBeReadIsExplained_data();
    void aProfileThatCouldNotBeReadIsExplained();
    void aHintedRetryIsAnsweredWithNoProfileInSight_data();
    void aHintedRetryIsAnsweredWithNoProfileInSight();
    void aHintedRetryKeepsTheSecretsItWasGiven();
    void theOneTimeCodeFieldIsWhereTheCursorIs();
    void aProfileAskingInsideAConnectionBlockStillAsksForAPassword();
};

void Openvpn3AuthTest::aChallengeHintAsksForAOneTimeCode()
{
    OpenVpn3AuthWidget widget(secretSetting(), {u"challenge-response"_s});

    QCOMPARE(keys(&widget), QStringList{u"challenge-response"_s});
}

void Openvpn3AuthTest::anEchoedChallengeIsStillASecret()
{
    OpenVpn3AuthWidget echoed(secretSetting(), {u"challenge-response"_s, u"x-challenge-echo"_s});
    OpenVpn3AuthWidget hidden(secretSetting(), {u"challenge-response"_s});

    // Echo only decides whether the code is readable while being typed; it
    // is returned as a secret either way.
    QCOMPARE(fields(&echoed).size(), 1);
    QCOMPARE(fields(&hidden).size(), 1);
    QCOMPARE(keys(&echoed), QStringList{u"challenge-response"_s});

    fields(&echoed).first()->setText(u"123456"_s);
    QCOMPARE(returnedSecrets(&echoed).value(u"challenge-response"_s), u"123456"_s);
}

void Openvpn3AuthTest::aChallengeIsNeverPrefilled()
{
    OpenVpn3AuthWidget widget(secretSetting(kProfile, {{u"challenge-response"_s, u"stale"_s}}), {u"challenge-response"_s});

    QCOMPARE(fields(&widget).first()->text(), QString());
}

void Openvpn3AuthTest::theServiceMessageBecomesTheLabel()
{
    OpenVpn3AuthWidget widget(secretSetting(), {u"challenge-response"_s, u"x-vpn-message:Enter your PIN."_s});

    QVERIFY(labels(&widget).contains(u"Enter your PIN:"_s));
}

void Openvpn3AuthTest::aCertPassHintAsksForThePassphrase()
{
    OpenVpn3AuthWidget widget(secretSetting(), {u"cert-pass"_s});

    QCOMPARE(keys(&widget), QStringList{u"cert-pass"_s});
    QCOMPARE(fields(&widget).first()->text(), QString());
}

void Openvpn3AuthTest::withoutHintsTheProfileDecides()
{
    OpenVpn3AuthWidget widget(secretSetting(kProfile, {{u"password"_s, u"pw"_s}}), {});

    QCOMPARE(keys(&widget), QStringList{u"password"_s});
    QCOMPARE(fields(&widget).first()->text(), u"pw"_s);
}

void Openvpn3AuthTest::aCertificateOnlyProfileAsksForNothing()
{
    OpenVpn3AuthWidget widget(secretSetting(kCertProfile), {});

    QVERIFY(fields(&widget).isEmpty());
    // Explain rather than show an empty dialog: the secrets it needs are the
    // ones already stored, the profile among them.
    QVERIFY(!labels(&widget).isEmpty());
}

void Openvpn3AuthTest::notRequiredSecretsAreNotAskedFor()
{
    OpenVpn3AuthWidget widget(secretSetting(kProfile, {}, {{u"password-flags"_s, QString::number(NetworkManager::Setting::NotRequired)}}), {});

    QVERIFY(!keys(&widget).contains(u"password"_s));
}

void Openvpn3AuthTest::storedSecretsAreHandedBackIncludingTheProfile()
{
    // The secret agent replaces the whole vpn setting with what this returns,
    // so leaving the profile out would take it away from the activation.
    OpenVpn3AuthWidget widget(secretSetting(kProfile, {{u"password"_s, u"pw"_s}}), {u"challenge-response"_s});

    const NMStringMap secrets = returnedSecrets(&widget);
    QCOMPARE(secrets.value(u"profile"_s), encoded(kProfile));
    QCOMPARE(secrets.value(u"password"_s), u"pw"_s);
}

void Openvpn3AuthTest::whatTheUserTypedIsAddedToTheStoredSecrets()
{
    OpenVpn3AuthWidget widget(secretSetting(kProfile, {{u"password"_s, u"pw"_s}}), {u"challenge-response"_s});

    fields(&widget).first()->setText(u"987654"_s);

    const NMStringMap secrets = returnedSecrets(&widget);
    QCOMPARE(secrets.value(u"challenge-response"_s), u"987654"_s);
    QCOMPARE(secrets.value(u"profile"_s), encoded(kProfile));
}

void Openvpn3AuthTest::nothingIsReturnedOutsideTheSecretsMap()
{
    OpenVpn3AuthWidget widget(secretSetting(), {u"challenge-response"_s});

    // Only secrets: the prompt must not rewrite the connection's data, which
    // is what decides whether a one-time code gets stored.
    QCOMPARE(widget.setting().keys(), QStringList{u"secrets"_s});
}

void Openvpn3AuthTest::aPlainPrivateKeyDoesNotAskForAPassphrase()
{
    const auto plain = QStringLiteral(
        "client\nremote vpn.example.org\n<key>\n-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n</key>\n");
    OpenVpn3AuthWidget widget(secretSetting(plain), {});

    QVERIFY(!keys(&widget).contains(u"cert-pass"_s));
}

void Openvpn3AuthTest::anEncryptedPrivateKeyAsksForAPassphrase()
{
    const auto encrypted = QStringLiteral(
        "client\nremote vpn.example.org\n<key>\n-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n-----END ENCRYPTED PRIVATE KEY-----\n</key>\n");
    OpenVpn3AuthWidget widget(secretSetting(encrypted), {});

    QCOMPARE(keys(&widget), QStringList{u"cert-pass"_s});
}

// -- a profile that is not here ------------------------------------------------

void Openvpn3AuthTest::aProfileThatCouldNotBeReadIsExplained_data()
{
    QTest::addColumn<NMStringMap>("data");
    QTest::addColumn<QString>("expected");

    QTest::newRow("locked") << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}} << u"wallet"_s;
    QTest::newRow("locked, and said to need a passphrase")
        << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}, {u"cert-pass-flags"_s, u"1"_s}} << u"wallet"_s;
    QTest::newRow("unsupported layout") << NMStringMap{{u"profile-storage"_s, u"v2-whatever"_s}} << u"version of Plasma"_s;
    QTest::newRow("flags no profile could be stored with")
        << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"2"_s}} << u"never stored"_s;
    QTest::newRow("no profile at all") << NMStringMap{} << u"no OpenVPN profile"_s;
}

void Openvpn3AuthTest::aProfileThatCouldNotBeReadIsExplained()
{
    QFETCH(NMStringMap, data);
    QFETCH(QString, expected);

    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    setting->setData(data);

    // NetworkManager's first, hintless request for a connection whose profile
    // the agent could not produce.
    OpenVpn3AuthWidget widget(setting, {});

    // What is missing is the profile; nothing typed into a password box could
    // be it, so asking for one would be a dialog that cannot help.
    QVERIFY(fields(&widget).isEmpty());
    const QStringList shown = labels(&widget);
    QVERIFY2(std::any_of(shown.cbegin(),
                         shown.cend(),
                         [&expected](const QString &text) {
                             return text.contains(expected);
                         }),
             qPrintable(shown.join(u" | "_s)));
}

void Openvpn3AuthTest::aHintedRetryIsAnsweredWithNoProfileInSight_data()
{
    QTest::addColumn<QStringList>("hints");
    QTest::addColumn<QString>("key");

    QTest::newRow("one-time code") << QStringList{u"challenge-response"_s} << u"challenge-response"_s;
    QTest::newRow("password") << QStringList{u"password"_s} << u"password"_s;
    QTest::newRow("passphrase") << QStringList{u"cert-pass"_s} << u"cert-pass"_s;
    QTest::newRow("one-time code with a message")
        << QStringList{u"challenge-response"_s, u"x-vpn-message:Enter the code from your token."_s} << u"challenge-response"_s;
}

void Openvpn3AuthTest::aHintedRetryIsAnsweredWithNoProfileInSight()
{
    QFETCH(QStringList, hints);
    QFETCH(QString, key);

    // What a retry looks like: NetworkManager asks with RequestNew, for which
    // the agent does not read the wallet, so the connection it hands over has
    // no profile secret -- even though the service is running and holding the
    // profile itself. Refusing here would make a mistyped password or an
    // expired one-time code the end of the connection.
    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    setting->setData({{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}});

    OpenVpn3AuthWidget widget(setting, hints);

    QCOMPARE(keys(&widget), QStringList{key});
    fields(&widget).first()->setText(u"typed"_s);
    QCOMPARE(returnedSecrets(&widget).value(key), u"typed"_s);
}

void Openvpn3AuthTest::aHintedRetryKeepsTheSecretsItWasGiven()
{
    // The agent replaces the whole vpn setting with what comes back, so a
    // retry that was handed the profile has to hand it on.
    OpenVpn3AuthWidget widget(secretSetting(kProfile, {{u"password"_s, u"pw"_s}}), {u"challenge-response"_s});

    fields(&widget).first()->setText(u"123456"_s);
    const NMStringMap secrets = returnedSecrets(&widget);

    QCOMPARE(secrets.value(u"challenge-response"_s), u"123456"_s);
    QCOMPARE(secrets.value(u"profile"_s), encoded(kProfile));
    QCOMPARE(secrets.value(u"password"_s), u"pw"_s);
}

void Openvpn3AuthTest::theOneTimeCodeFieldIsWhereTheCursorIs()
{
    OpenVpn3AuthWidget widget(secretSetting(kProfile, {{u"password"_s, u"pw"_s}}), {u"challenge-response"_s});
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    // One field, empty, and the thing the user is being asked for: typing
    // should not need a click first.
    QCOMPARE(fields(&widget).size(), 1);
    QVERIFY(fields(&widget).first()->isAncestorOf(widget.focusWidget()) || fields(&widget).first() == widget.focusWidget());
}

void Openvpn3AuthTest::aProfileAskingInsideAConnectionBlockStillAsksForAPassword()
{
    // openvpn3 reads <connection> as options, so the password it asks for
    // there is one the prompt has to offer.
    const auto nested = QStringLiteral("client\n<connection>\nremote vpn.example.org 1194\nauth-user-pass\n</connection>\n");
    OpenVpn3AuthWidget widget(secretSetting(nested), {});

    QCOMPARE(keys(&widget), QStringList{u"password"_s});
}

QTEST_MAIN(Openvpn3AuthTest)

#include "openvpn3authtest.moc"
