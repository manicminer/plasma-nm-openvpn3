/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QMessageBox>
#include <QTimer>

#include <utility>

#include <NetworkManagerQt/VpnSetting>

#include "nm-openvpn3-service.h"
#include "openvpn3backendguard.h"
#include "openvpn3importer.h"
#include "openvpn3profile.h"
#include "openvpn3storage.h"
#include "openvpn3widget.h"
#include "passwordfield.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
const auto kProfile = QStringLiteral(
    "client\n"
    "dev tun\n"
    "proto udp\n"
    "port 1194\n"
    "remote vpn1.example.org 1194 udp\n"
    "remote vpn2.example.org 443 tcp\n"
    "<connection>\n"
    "remote fallback.example.org 1194 udp\n"
    "</connection>\n"
    "auth-user-pass\n"
    "setenv opt 'single quoted value'\n"
    "some-directive-we-have-never-heard-of 1 2 3\n"
    "<ca>\n"
    "-----BEGIN CERTIFICATE-----\n"
    "SYNTHETIC\n"
    "-----END CERTIFICATE-----\n"
    "</ca>\n");

/** A profile as the backend imports one: comments and blank lines the editor
 * keeps without showing them as rows. */
const auto kCommented = QStringLiteral(
    "##\n"
    "# Synthetic test profile\n"
    "##\n"
    "\n"
    "client\n"
    "remote vpn1.example.org 1194 udp # the main one\n"
    "; and a semicolon comment\n"
    "   \n"
    "setenv hash \"a # inside quotes\"\n"
    "\n"
    "<ca>\n"
    "-----BEGIN CERTIFICATE-----\n"
    "SYNTHETIC # payload, not a comment\n"
    "\n"
    "-----END CERTIFICATE-----\n"
    "</ca>\n"
    "\n");

/** kCommented as openvpn3 reads it, Openvpn3Profile::optionsText(): what the
 * rows of the table say. The stored document is still kCommented. */
const auto kCommentedKept = QStringLiteral(
    "client\n"
    "remote vpn1.example.org 1194 udp\n"
    "setenv hash \"a # inside quotes\"\n"
    "<ca>\n"
    "-----BEGIN CERTIFICATE-----\n"
    "SYNTHETIC # payload, not a comment\n"
    "\n"
    "-----END CERTIFICATE-----\n"
    "</ca>\n");

QString encoded(const QString &text)
{
    return QString::fromLatin1(text.toUtf8().toBase64());
}

/**
 * A VPN setting the way the connection editor hands one over: built from a
 * D-Bus map, so that NetworkManagerQt considers it initialised.
 */
NetworkManager::VpnSetting::Ptr settingFrom(const NMStringMap &data, const NMStringMap &secrets = {})
{
    NetworkManager::VpnSetting source;
    source.setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    source.setData(data);
    source.setSecrets(secrets);

    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->fromMap(source.toMap());
    setting->setSecrets(secrets); // fromMap() does not have to carry them
    return setting;
}

/** A VPN setting in the legacy public-profile layout. */
NetworkManager::VpnSetting::Ptr legacySetting(const QString &profile = kProfile)
{
    return settingFrom({{u"profile"_s, encoded(profile)}, {u"username"_s, u"alice"_s}, {u"password-flags"_s, u"1"_s}});
}

/** A VPN setting whose profile is one of the connection's secrets, in the
 * layout the proposal before the backend's 0.2.0 wrote: the explicit
 * marker as well as the flags. Still read; see upstreamSetting(). */
NetworkManager::VpnSetting::Ptr secretSetting(const QString &flags = u"1"_s, bool withSecret = true, const QString &profile = kProfile)
{
    const NMStringMap data{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, flags}, {u"username"_s, u"alice"_s}};
    NMStringMap secrets;
    if (withSecret) {
        secrets.insert(u"profile"_s, encoded(profile));
    }
    return settingFrom(data, secrets);
}

/** A VPN setting in the backend's own layout (0.2.0 and later): the flags are
 * the only marker, and the first remote is there for display. */
NetworkManager::VpnSetting::Ptr upstreamSetting(const QString &flags = u"0"_s, bool withSecret = true, const QString &profile = kProfile)
{
    const NMStringMap data{{u"profile-flags"_s, flags}, {u"remote"_s, u"vpn1.example.org"_s}, {u"username"_s, u"alice"_s}, {u"x-other"_s, u"keep"_s}};
    NMStringMap secrets;
    if (withSecret) {
        secrets.insert(u"profile"_s, encoded(profile));
    }
    return settingFrom(data, secrets);
}

NMStringMap dataOf(const QVariantMap &map)
{
    return qdbus_cast<NMStringMap>(map.value(u"data"_s));
}

NMStringMap secretsOf(const QVariantMap &map)
{
    return qdbus_cast<NMStringMap>(map.value(u"secrets"_s));
}

QString storedProfileOf(const QVariantMap &map)
{
    return Openvpn3Storage::readProfile(dataOf(map), secretsOf(map));
}

QTableWidget *remotesTable(OpenVpn3SettingWidget *widget)
{
    return widget->findChild<QTableWidget *>(u"openvpn3_remotes"_s);
}

QPlainTextEdit *sourceEdit(OpenVpn3SettingWidget *widget)
{
    return widget->findChild<QPlainTextEdit *>(u"openvpn3_source"_s);
}

QComboBox *storageCombo(OpenVpn3SettingWidget *widget)
{
    return widget->findChild<QComboBox *>(u"openvpn3_storage"_s);
}

QLineEdit *portEdit(OpenVpn3SettingWidget *widget)
{
    return widget->findChild<QLineEdit *>(u"openvpn3_port"_s);
}

QTabWidget *tabs(OpenVpn3SettingWidget *widget)
{
    return widget->findChild<QTabWidget *>(u"openvpn3_tabs"_s);
}

QTableWidget *directivesTable(OpenVpn3SettingWidget *widget)
{
    return widget->findChild<QTableWidget *>(u"openvpn3_directives_table"_s);
}

/** The row of the first entry called @p name in the directive table. */
int rowOfDirective(const QTableWidget *table, const QString &name)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 1) && table->item(row, 1)->text() == name) {
            return row;
        }
    }
    return -1;
}

QString dataPath(const QString &name)
{
    return QString::fromLatin1(OPENVPN3_TEST_DATA_DIR) + QLatin1Char('/') + name;
}

}

/** The editor page: what it shows, what it stores, and what it refuses to do. */
class Openvpn3WidgetTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void delayedSecretsRespectCredentialEdits_data()
    {
        QTest::addColumn<QString>("editedKey");
        QTest::addColumn<bool>("editText");
        QTest::newRow("password-text") << u"password"_s << true;
        QTest::newRow("password-policy") << u"password"_s << false;
        QTest::newRow("passphrase-text") << u"cert-pass"_s << true;
        QTest::newRow("passphrase-policy") << u"cert-pass"_s << false;
    }
    void delayedSecretsRespectCredentialEdits()
    {
        QFETCH(QString, editedKey);
        QFETCH(bool, editText);
        auto setting = legacySetting(kProfile + u"<key>\n-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n</key>\n"_s);
        auto data = setting->data();
        data[u"password-flags"_s] = u"0"_s;
        data[u"cert-pass-flags"_s] = u"0"_s;
        setting->setData(data);
        OpenVpn3SettingWidget widget(setting);
        auto password = widget.findChild<PasswordField *>(u"openvpn3_password"_s);
        auto passphrase = widget.findChild<PasswordField *>(u"openvpn3_certpass"_s);
        QVERIFY(password);
        QVERIFY(passphrase);
        auto edited = editedKey == u"password"_s ? password : passphrase;
        if (editText) {
            edited->setText(u"new-value"_s);
        } else {
            edited->setPasswordOption(PasswordField::AlwaysAsk);
        }
        widget.findChild<QLineEdit *>(u"openvpn3_username"_s)->setText(u"bob"_s);
        widget.loadSecrets(settingFrom(data, {{u"password"_s, u"old-password"_s}, {u"cert-pass"_s, u"old-passphrase"_s}}));
        if (editText) {
            QCOMPARE(edited->text(), u"new-value"_s);
            QCOMPARE(secretsOf(widget.setting()).value(editedKey), u"new-value"_s);
        } else {
            QCOMPARE(edited->passwordOption(), PasswordField::AlwaysAsk);
            QVERIFY(!secretsOf(widget.setting()).contains(editedKey));
        }
        auto untouched = edited == password ? passphrase : password;
        QCOMPARE(untouched->text(), edited == password ? u"old-passphrase"_s : u"old-password"_s);
        QCOMPARE(dataOf(widget.setting()).value(u"username"_s), u"bob"_s);
    }

    void delayedSecretsAfterReimport_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<bool>("succeed");
        for (const auto &[name, layout] : {std::pair{"legacy", 0}, {"secret-locked", 1}, {"secret-loaded", 2}}) {
            QTest::newRow(qPrintable(QString::fromLatin1(name) + u"-success"_s)) << layout << true;
            QTest::newRow(qPrintable(QString::fromLatin1(name) + u"-failure"_s)) << layout << false;
        }
    }

    void delayedSecretsAfterReimport()
    {
        QFETCH(int, layout);
        QFETCH(bool, succeed);
        // These regressions go through the real importer, so they skip with
        // the rest of them where the backend is not installed rather than
        // failing. A run with no skips is what says the real importer was
        // exercised; a run that failed here would say nothing at all.
        OPENVPN3_REQUIRE_BACKEND();
        auto original = layout == 0 ? legacySetting() : secretSetting(u"0"_s, layout == 2);
        auto data = original->data();
        data[u"username"_s] = u"original-user"_s;
        data[u"password-flags"_s] = u"1"_s;
        data[u"cert-pass-flags"_s] = u"1"_s;
        original->setData(data);
        original->setTimeout(180);
        original->setPersistent(true);
        original->setUsername(u"vpn-owner"_s);
        const auto originalMap = original->toMap();
        OpenVpn3SettingWidget widget(original);
        // Preserve unsaved storage choices through replacement and the late reply.
        storageCombo(&widget)->setCurrentIndex(storageCombo(&widget)->findData(int(NetworkManager::Setting::None)));
        auto password = widget.findChild<PasswordField *>(u"openvpn3_password"_s);
        auto passphrase = widget.findChild<PasswordField *>(u"openvpn3_certpass"_s);
        QVERIFY(password);
        QVERIFY(passphrase);
        password->setPasswordOption(PasswordField::StoreForAllUsers);
        const auto before = widget.setting();
        QString error;
        QCOMPARE(widget.importProfile(succeed ? dataPath(u"office.ovpn"_s) : u"/nonexistent/nowhere.ovpn"_s, &error), succeed);
        if (!succeed) {
            QVERIFY(!error.isEmpty());
            QCOMPARE(widget.setting(), before);
        } else {
            QVERIFY(widget.isValid());
            QCOMPARE(dataOf(widget.setting()).value(u"profile-flags"_s), u"0"_s);
            QCOMPARE(dataOf(widget.setting()).value(u"password-flags"_s), u"0"_s);
            QCOMPARE(secretsOf(widget.setting()).value(u"password"_s), u"correct-horse-battery-staple"_s);
        }
        const auto imported = widget.setting();
        const auto reply = settingFrom(data, {{u"profile"_s, encoded(kProfile)},
                                              {u"password"_s, u"old-password"_s},
                                              {u"cert-pass"_s, u"old-passphrase"_s},
                                              {u"old-extra-secret"_s, u"old-value"_s}});
        widget.loadSecrets(reply);
        if (succeed) {
            QCOMPARE(password->text(), u"correct-horse-battery-staple"_s);
            QVERIFY(passphrase->text().isEmpty());
            QCOMPARE(dataOf(widget.setting()), dataOf(imported));
            QCOMPARE(secretsOf(widget.setting()), secretsOf(imported));
            QCOMPARE(widget.setting(), imported);
            // A failed subsequent import must not revive the original replies.
            QVERIFY(!widget.importProfile(u"/nonexistent/nowhere.ovpn"_s));
            widget.loadSecrets(reply);
            QCOMPARE(widget.setting(), imported);
            // An actual fresh load starts accepting its pending secrets again.
            widget.loadConfig(original);
            widget.loadSecrets(reply);
        }
        QCOMPARE(password->text(), u"old-password"_s);
        QCOMPARE(passphrase->text(), u"old-passphrase"_s);
        QCOMPARE(secretsOf(widget.setting()).value(u"password"_s), u"old-password"_s);
        QCOMPARE(storedProfileOf(widget.setting()), kProfile);
        QCOMPARE(dataOf(widget.setting()).value(u"username"_s), u"original-user"_s);
        for (const auto &key : {u"timeout"_s, u"persistent"_s, u"user-name"_s, u"service-type"_s}) {
            QCOMPARE(imported.value(key), originalMap.value(key));
            QCOMPARE(widget.setting().value(key), originalMap.value(key));
        }
        QCOMPARE(original->toMap(), originalMap);
    }

    void everyEffectiveRemoteNeedsAHost_data()
    {
        QTest::addColumn<QString>("extra");
        QTest::newRow("add-row") << QString();
        QTest::newRow("top-level") << u"remote \"\"\n"_s;
        QTest::newRow("scoped") << u"<connection>\nremote\n</connection>\n"_s;
    }
    void everyEffectiveRemoteNeedsAHost()
    {
        QFETCH(QString, extra);
        OpenVpn3SettingWidget widget(secretSetting(u"0"_s, true, u"client\nremote valid.example.org\n"_s + extra));
        if (extra.isEmpty()) {
            QVERIFY(widget.isValid());
            widget.findChild<QPushButton *>(u"openvpn3_remote_add"_s)->click();
        }
        QVERIFY(!widget.isValid());
        QVERIFY(widget.blockingProblem().contains(u"host"_s));
    }

    void explicitEmptyReplacementPasswordClearsOldSecret()
    {
        OPENVPN3_REQUIRE_BACKEND();
        for (int destination : {2, 0, 1}) {
            auto setting = secretSetting();
            auto secrets = setting->secrets();
            secrets.insert(u"password"_s, u"old-server-password"_s);
            setting->setSecrets(secrets);
            OpenVpn3SettingWidget widget(setting);
            tabs(&widget)->setCurrentIndex(2);
            sourceEdit(&widget)->setPlainText(u"client\nremote new.example.org\n<auth-user-pass>\nbob\n\n</auth-user-pass>\n"_s);
            tabs(&widget)->setCurrentIndex(destination);
            QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
            QCOMPARE(dataOf(widget.setting()).value(u"username"_s), u"bob"_s);
            QVERIFY(!secretsOf(widget.setting()).contains(u"password"_s));
        }
    }

    void normalizationUsesTheOutgoingTab()
    {
        OPENVPN3_REQUIRE_BACKEND();
        for (int destination : {0, 1}) {
            OpenVpn3SettingWidget widget(secretSetting());
            tabs(&widget)->setCurrentIndex(2);
            sourceEdit(&widget)->setPlainText(u"client\nremote new.example.org\n<auth-user-pass>\nnew-user\nnew-password\n</auth-user-pass>\n"_s);
            tabs(&widget)->setCurrentIndex(destination);
            QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
            QCOMPARE(dataOf(widget.setting()).value(u"username"_s), u"new-user"_s);
            QCOMPARE(secretsOf(widget.setting()).value(u"password"_s), u"new-password"_s);
            QVERIFY(storedProfileOf(widget.setting()).contains(u"new.example.org"_s));
            QVERIFY(!storedProfileOf(widget.setting()).contains(u"new-password"_s));
        }
    }

    void credentialsLiftedOutOfTheProfileSurviveALateReply_data()
    {
        QTest::addColumn<QString>("stored");
        QTest::newRow("an older stored password") << u"old-password"_s;
        QTest::newRow("no stored password at all") << QString();
    }
    /**
     * A credential moved out of the profile into its field is an edit, and a
     * secrets reply that was already on its way must not undo it.
     *
     * Writing credentials into Profile Source and leaving the tab normalizes
     * the document: the password ends up in the field and is no longer in the
     * profile, so the field is the only thing holding it. A reply arriving
     * afterwards would otherwise put the stored password back, or clear the
     * field when there was none, and the connection would be saved with
     * credentials the user never entered.
     */
    void credentialsLiftedOutOfTheProfileSurviveALateReply()
    {
        QFETCH(QString, stored);
        OPENVPN3_REQUIRE_BACKEND();
        OpenVpn3SettingWidget widget(secretSetting());
        tabs(&widget)->setCurrentIndex(2);
        sourceEdit(&widget)->setPlainText(u"client\nremote new.example.org\n<auth-user-pass>\nnew-user\nnew-password\n</auth-user-pass>\n"_s);
        tabs(&widget)->setCurrentIndex(0);
        QCOMPARE(secretsOf(widget.setting()).value(u"password"_s), u"new-password"_s);

        NMStringMap late;
        if (!stored.isNull()) {
            late.insert(u"password"_s, stored);
        }
        widget.loadSecrets(settingFrom({}, late));

        auto password = widget.findChild<PasswordField *>(u"openvpn3_password"_s);
        QVERIFY(password);
        QCOMPARE(password->text(), u"new-password"_s);
        QCOMPARE(secretsOf(widget.setting()).value(u"password"_s), u"new-password"_s);
        QCOMPARE(dataOf(widget.setting()).value(u"username"_s), u"new-user"_s);
    }

    /**
     * A secret-mode connection with no profile-flags entry says why the
     * profile may not have arrived, because the reason is not in the editor.
     *
     * A connection editor asks NetworkManager for the secrets a connection
     * records that it keeps, and the record is the -flags entries. Nothing
     * this plugin writes is ever missing profile-flags, but a connection
     * written by something else can be, and then the profile is simply never
     * requested -- which looks exactly like a locked wallet from in here.
     */
    void aSecretProfileWithoutFlagsSaysWhyItMayNotHaveArrived()
    {
        OpenVpn3SettingWidget widget(settingFrom({{u"profile-storage"_s, u"secret"_s}}));
        QVERIFY(!widget.isValid());
        const QString problem = widget.blockingProblem();
        QVERIFY2(problem.contains(u"profile-flags"_s), qPrintable(problem));
        QVERIFY2(problem.contains(u"nmcli"_s), qPrintable(problem));

        // A connection that does record them is locked for some other reason,
        // and guessing at this one would only mislead.
        OpenVpn3SettingWidget recorded(settingFrom({{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}}));
        QVERIFY(!recorded.isValid());
        QVERIFY(!recorded.blockingProblem().contains(u"profile-flags"_s));

        // Nor does a missing entry mean the secrets were not asked for: the
        // host asks for all of a connection's VPN secrets when any one of its
        // -flags entries says a secret is kept, so one of those is enough and
        // the profile arrives with it. Saying otherwise would send somebody
        // off to change an entry that was not the problem.
        for (const QString &flags : {u"0"_s, u"1"_s, u"not-a-number"_s}) {
            OpenVpn3SettingWidget asked(settingFrom({{u"profile-storage"_s, u"secret"_s}, {u"password-flags"_s, flags}}));
            QVERIFY(!asked.isValid());
            QVERIFY2(!asked.blockingProblem().contains(u"profile-flags"_s), qPrintable(flags + u": "_s + asked.blockingProblem()));
        }
        // ... but a connection whose only other entry says nothing is kept is
        // one the host really does not ask about.
        OpenVpn3SettingWidget unasked(settingFrom({{u"profile-storage"_s, u"secret"_s}, {u"password-flags"_s, u"4"_s}}));
        QVERIFY(!unasked.isValid());
        QVERIFY(unasked.blockingProblem().contains(u"profile-flags"_s));
    }

    void normalizedPasswordCannotBypassUnavailableWallet()
    {
        OPENVPN3_REQUIRE_BACKEND();
        Openvpn3Storage::setSecretServiceAvailability(false);
        OpenVpn3SettingWidget widget(secretSetting(u"0"_s));
        auto password = widget.findChild<PasswordField *>(u"openvpn3_password"_s);
        password->setPasswordOption(PasswordField::StoreForUser);
        tabs(&widget)->setCurrentIndex(2);
        sourceEdit(&widget)->setPlainText(u"client\nremote example.org\n<auth-user-pass>\nalice\nsynthetic\n</auth-user-pass>\n"_s);
        QVERIFY(!widget.isValid());
        QVERIFY(widget.blockingProblem().contains(u"wallet"_s));
    }
    void blockedOutputStillDropsOldOtp()
    {
        auto setting = secretSetting();
        auto secrets = setting->secrets();
        secrets.insert(u"challenge-response"_s, u"old-otp"_s);
        setting->setSecrets(secrets);
        Openvpn3Storage::setSecretServiceAvailability(false);
        OpenVpn3SettingWidget widget(setting);
        QVERIFY(!widget.isValid());
        QVERIFY(!secretsOf(widget.setting()).contains(u"challenge-response"_s));
    }

    void typedFilesSurviveDeletionAndReload()
    {
        OPENVPN3_REQUIRE_BACKEND();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(u"ca.pem"_s);
        QVERIFY(QFile::copy(dataPath(u"pki/ca.crt"_s), path));
        OpenVpn3SettingWidget widget(secretSetting());
        tabs(&widget)->setCurrentIndex(2);
        sourceEdit(&widget)->setPlainText(u"client\nremote example.org\nca "_s + path + u"\n"_s);
        tabs(&widget)->setCurrentIndex(0);
        QVERIFY(QFile::remove(path));
        QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
        const auto map = widget.setting();
        auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
        setting->fromMap(map);
        setting->setSecrets(secretsOf(map));
        OpenVpn3SettingWidget reloaded(setting);
        QVERIFY(reloaded.isValid());
        QVERIFY(storedProfileOf(reloaded.setting()).contains(u"<ca>"_s));
        QVERIFY(!storedProfileOf(reloaded.setting()).contains(path));
    }

    void normalizedCredentialsCanBeEditedAfterSwitchingTabs()
    {
        OPENVPN3_REQUIRE_BACKEND();
        OpenVpn3SettingWidget widget(secretSetting());
        tabs(&widget)->setCurrentIndex(2);
        sourceEdit(&widget)->setPlainText(u"client\nremote example.org\n<auth-user-pass>\nbob\nold-password\n</auth-user-pass>\n"_s);
        tabs(&widget)->setCurrentIndex(0);
        widget.findChild<QLineEdit *>(u"openvpn3_username"_s)->setText(u"alice"_s);
        widget.findChild<PasswordField *>(u"openvpn3_password"_s)->setText(u"new-password"_s);
        QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
        QCOMPARE(dataOf(widget.setting()).value(u"username"_s), u"alice"_s);
        QCOMPARE(secretsOf(widget.setting()).value(u"password"_s), u"new-password"_s);
        QVERIFY(!storedProfileOf(widget.setting()).contains(u"old-password"_s));
    }
    void cancellingReimportPreservesFieldOnlyChanges()
    {
        for (bool storage : {false, true}) {
            OpenVpn3SettingWidget widget(secretSetting());
            if (storage) {
                storageCombo(&widget)->setCurrentIndex(storageCombo(&widget)->findData(0));
            } else {
                widget.findChild<PasswordField *>(u"openvpn3_password"_s)->setText(u"changed"_s);
            }
            const auto before = widget.setting();
            bool prompted = false;
            QTimer::singleShot(0, &widget, [&] {
                auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (box) {
                    prompted = true;
                    box->done(QMessageBox::Cancel);
                }
            });
            widget.findChild<QPushButton *>(u"openvpn3_import"_s)->click();
            QVERIFY(prompted);
            QCOMPARE(widget.setting(), before);
            QVERIFY(widget.hasUnsavedEdits());
            widget.loadSecrets(settingFrom({}, {{u"password"_s, u"late-password"_s}, {u"cert-pass"_s, u"late-passphrase"_s}}));
            QCOMPARE(widget.findChild<PasswordField *>(u"openvpn3_password"_s)->text(), storage ? u"late-password"_s : u"changed"_s);
            QCOMPARE(widget.findChild<PasswordField *>(u"openvpn3_certpass"_s)->text(), u"late-passphrase"_s);
            QCOMPARE(dataOf(widget.setting()), dataOf(before));
            QCOMPARE(storedProfileOf(widget.setting()), storedProfileOf(before));
        }
    }
    void namedFieldPreservesOtherArgumentsAndDuplicates()
    {
        OpenVpn3SettingWidget widget(legacySetting(u"client\nremote example.org\ndev tun extra\ndev other\n"_s));
        auto dev = widget.findChild<QComboBox *>(u"openvpn3_device"_s);
        QVERIFY(dev);
        dev->setEditText(u"tap"_s);
        QCOMPARE(storedProfileOf(widget.setting()), u"client\nremote example.org\ndev tap extra\ndev other\n"_s);
        dev->setEditText(QString());
        QCOMPARE(storedProfileOf(widget.setting()), u"client\nremote example.org\ndev other\n"_s);
    }
    void invalidSourceShowsActionableStatusImmediately()
    {
        OpenVpn3SettingWidget widget(secretSetting());
        tabs(&widget)->setCurrentIndex(2);
        sourceEdit(&widget)->setPlainText(u"client\nremote example.org\nca /nonexistent/cert.pem\n"_s);
        QVERIFY(!widget.isValid());
        QVERIFY(widget.findChild<QLabel *>(u"openvpn3_status"_s)->text().contains(u"self-contained"_s));
    }

    void initTestCase();
    void init();

    void untouchedLegacyProfileRoundTripsByteForByte();
    void untouchedSecretProfileRoundTrips();
    void legacyConnectionIsNotMovedToTheWalletBehindTheUsersBack();
    void theStorageChoiceDrivesTheStoredLayout();
    void anExplicitSystemChoiceIsRespected();
    void editingAPortChangesOnlyThatLine();
    void editingARemoteKeepsItsOtherArguments();
    void addingARemoteAppendsIt();
    void removingARemoteLeavesConnectionBlocksAlone();
    void sourceEditsAreTakenVerbatim();
    void aCommentTypedIntoTheSourceIsKept();
    void theSourceShowsTheStoredDocumentVerbatim();
    void visitingThePagesNeverRewritesTheDocument();
    void tableAndFieldEditsKeepTheFormatting();
    void aCrlfDocumentStaysCrlfThroughEdits();
    void anUpstreamConnectionIsSavedInTheUpstreamLayout();
    void aMarkerConnectionIsMovedToTheUpstreamLayout_data();
    void aMarkerConnectionIsMovedToTheUpstreamLayout();
    void staleOrUnknownLayoutsFailClosed_data();
    void staleOrUnknownLayoutsFailClosed();
    void lateSecretsCannotOverwriteAnEditedProfile();
    void connectionBlockCaveatsAreShown();
    void resolvingAReferenceTouchesOnlyThatEntry();
    void aRelativePathIsExplained();
    void editingAServerKeepsWhatOpenvpn3ReadsFromTheLine();
    void aMissingFileThatAppearsIsPickedUpWithoutAnEdit();
    void aChangedOrDeletedFileIsReadAgainOnSave();
    void textInsideAnOpaqueBlockIsNeverResolved();
    void visitingThePagesNeverResolvesWhatWasLoaded();
    void removingARowKeepsItsCommentThroughSaveAndReload();
    void credentialsAreTheSameWithOrWithoutATabSwitch();
    void sourceEditsReachTheNamedFields();
    void aLockedProfileIsNeverOverwritten();
    void anUnknownStorageLayoutBlocksSaving();
    void unusableProfileFlagsBlockSaving();
    void loadSecretsFillsInALockedProfile();
    void loadSecretsWithoutTheProfileDoesNotBlankIt();
    void aProfileWithoutARemoteIsNotValid();
    void aRemoteInsideAConnectionBlockCounts();
    void oneTimeCodesAreMarkedAsNeverStored();
    void nothingSensitiveEverReachesTheDataMap();

    void reimportReplacesOnlyTheVpnConfiguration();
    void aFailedReimportChangesNothing();
    void reimportKeepsAnExplicitSystemStorageChoice();
    void editingMarksTheProfileSoAReimportCanWarn();
    void vpnPropertiesThisPageDoesNotShowSurviveASave();
    void storedSecretsAreNotShownInClear();

    void directiveTableEditsReachTheStoredProfile();
    void directiveTableKeepsEntriesTheEditorDoesNotUnderstand();
    void commentsInAStoredProfileAreKeptButNotShownAsRows();
    void blankLinesInAStoredProfileAreKeptButNotShownAsRows();
    void aCommentTypedIntoAScopeBodyIsSaved();
    void aBlankLineTypedIntoTheSourceIsSaved();
    void aBlankLineTypedIntoAScopeBodyIsSaved();
    void aBlockBodyCanBeEditedInTheTable();

    void removingARemoteDoesNotMoveAnothersExtraArguments();
    void reorderingRemotesCarriesTheirExtraArguments();

    void aProfileWithoutKeyMaterialStoresNoPassphraseKeys();
    void aProfileWithAnEncryptedKeyStoresThePassphraseFlags();

    // -- what the connection editor is told about an edit ---------------------
    void aSourceEditEnablesSaving();
    void aDirectiveTableEditEnablesSaving();
    void aBlockBodyEditEnablesSaving();
    void aCredentialEditCountsAsAnUnsavedChange();
    void aStorageChoiceCountsAsAnUnsavedChange();

    // -- the profile source page ---------------------------------------------
    void visitingEveryTabChangesNothingInACrlfProfile();
    void anEditedCrlfProfileKeepsItsLineEndings();

    // -- servers --------------------------------------------------------------
    void aBlankServerRowIsNotSomethingToSave();
    void removingAServerLeavesTheOtherDirectivesWhereTheyWere();
    void reorderingServersMovesOnlyTheServers();

    // -- scoped directives ----------------------------------------------------
    void credentialsForAProfileThatAsksInsideAConnectionBlockSurvive();
    void theAuthCheckboxSaysWhenTheDirectiveIsNotItsToChange();
    void aNestedInlineCertificateStaysOpaque();

    // -- secrets --------------------------------------------------------------
    void aStaleOneTimeCodeIsNeverSavedAgain();
    void aWalletThatCannotStoreAnythingBlocksTheSave();
    void anExplicitSystemChoiceWorksWithoutAWallet();
    void aWalletLessPasswordIsNeverQuietlyStoredForEveryone();
    void publicProfileStorageIsNotOfferedForASecretProfile();
    void anExistingPublicProfileKeepsItsChoice();

    // -- certificates and keys ------------------------------------------------
    void embeddingAFileReplacesTheReferenceToIt();
    void embeddingATlsAuthKeyKeepsItsKeyDirection_data();
    void embeddingATlsAuthKeyKeepsItsKeyDirection();
    void aBinaryFileIsRefusedRatherThanMangled();
    void aPkcs12BundleIsEmbeddedAsBase64();
    void clearingRemovesEveryTraceOfTheDirective();

    // -- normalisation --------------------------------------------------------
    void aTypedFileReferenceIsEmbeddedOnSave();
    void typedInlineCredentialsBecomeConnectionCredentials();
    void aProfileNamingAFileThatIsNotThereIsNotSaved();
    void anUntouchedProfileIsNeverRewritten();

    // -- reimport -------------------------------------------------------------
    void aReimportKeepsAnUnsavedStorageChoice();
    void aReimportIsWorthConfirmingWhenCredentialsAreStored();
};

void Openvpn3WidgetTest::initTestCase()
{
    // Every test says what it needs; the machine the tests run on does not
    // get to decide whether a wallet is there.
    Openvpn3Storage::setSecretServiceAvailability(true);
}

void Openvpn3WidgetTest::init()
{
    Openvpn3Storage::setSecretServiceAvailability(true);
}

void Openvpn3WidgetTest::untouchedLegacyProfileRoundTripsByteForByte()
{
    OpenVpn3SettingWidget widget(legacySetting());

    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
}

void Openvpn3WidgetTest::untouchedSecretProfileRoundTrips()
{
    OpenVpn3SettingWidget widget(secretSetting());
    const QVariantMap map = widget.setting();

    QCOMPARE(storedProfileOf(map), kProfile);
    // Saved in the backend's layout: the flags kept, the older marker gone.
    QVERIFY(!dataOf(map).contains(u"profile-storage"_s));
    QCOMPARE(dataOf(map).value(u"profile-flags"_s), u"1"_s);
    QCOMPARE(dataOf(map).value(u"remote"_s), u"vpn1.example.org"_s);
    QVERIFY(!dataOf(map).contains(u"profile"_s));
}

void Openvpn3WidgetTest::legacyConnectionIsNotMovedToTheWalletBehindTheUsersBack()
{
    OpenVpn3SettingWidget widget(legacySetting());
    const QVariantMap map = widget.setting();

    // Opening an existing connection and saving it again must not relocate
    // its profile; the storage combo is how the user asks for that.
    QVERIFY(dataOf(map).contains(u"profile"_s));
    QVERIFY(!dataOf(map).contains(u"profile-storage"_s));
    QVERIFY(!secretsOf(map).contains(u"profile"_s));
    QCOMPARE(storageCombo(&widget)->currentData().toInt(), -1);
}

void Openvpn3WidgetTest::theStorageChoiceDrivesTheStoredLayout()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QComboBox *combo = storageCombo(&widget);
    QVERIFY(combo);

    combo->setCurrentIndex(combo->findData(static_cast<int>(NetworkManager::Setting::AgentOwned)));
    QVariantMap map = widget.setting();
    QCOMPARE(dataOf(map).value(u"profile-flags"_s), u"1"_s);
    QVERIFY(!dataOf(map).contains(u"profile"_s));
    QCOMPARE(storedProfileOf(map), kProfile);

    combo->setCurrentIndex(combo->findData(static_cast<int>(NetworkManager::Setting::None)));
    map = widget.setting();
    QCOMPARE(dataOf(map).value(u"profile-flags"_s), u"0"_s);
    QCOMPARE(storedProfileOf(map), kProfile);
}

void Openvpn3WidgetTest::anExplicitSystemChoiceIsRespected()
{
    OpenVpn3SettingWidget widget(secretSetting(u"0"_s));

    QCOMPARE(storageCombo(&widget)->currentData().toInt(), static_cast<int>(NetworkManager::Setting::None));
    QCOMPARE(dataOf(widget.setting()).value(u"profile-flags"_s), u"0"_s);
}

void Openvpn3WidgetTest::editingAPortChangesOnlyThatLine()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QLineEdit *port = portEdit(&widget);
    QVERIFY(port);
    QCOMPARE(port->text(), u"1194"_s);

    port->setText(u"1195"_s);
    Q_EMIT port->editingFinished();

    const QString stored = storedProfileOf(widget.setting());
    QCOMPARE(stored, QString(kProfile).replace(u"port 1194\n"_s, u"port 1195\n"_s));
}

void Openvpn3WidgetTest::editingARemoteKeepsItsOtherArguments()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QTableWidget *table = remotesTable(&widget);
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 0)->text(), u"vpn1.example.org"_s);
    QCOMPARE(table->item(1, 2)->text(), u"tcp"_s);

    table->item(0, 0)->setText(u"vpn3.example.org"_s);

    const QString stored = storedProfileOf(widget.setting());
    QVERIFY(stored.contains(u"remote vpn3.example.org 1194 udp\n"_s));
    QVERIFY(stored.contains(u"remote vpn2.example.org 443 tcp\n"_s));
}

void Openvpn3WidgetTest::addingARemoteAppendsIt()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QTableWidget *table = remotesTable(&widget);
    const int rows = table->rowCount();

    table->insertRow(rows);
    table->setItem(rows, 0, new QTableWidgetItem(u"vpn4.example.org"_s));

    const Openvpn3Profile profile = Openvpn3Profile::fromText(storedProfileOf(widget.setting()));
    const QList<int> remotes = profile.indexesOf(u"remote"_s);
    QCOMPARE(remotes.size(), 3);
    QCOMPARE(profile.at(remotes.last()).value(), u"vpn4.example.org"_s);
}

void Openvpn3WidgetTest::removingARemoteLeavesConnectionBlocksAlone()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QTableWidget *table = remotesTable(&widget);

    table->removeRow(1);
    // removeRow() does not emit cellChanged; setItem() does, which is the
    // same path the Remove button takes.
    table->setItem(0, 0, new QTableWidgetItem(u"vpn1.example.org"_s));

    const QString stored = storedProfileOf(widget.setting());
    QVERIFY(stored.contains(u"remote fallback.example.org 1194 udp"_s));
    QVERIFY(stored.contains(u"<connection>"_s));
}

void Openvpn3WidgetTest::sourceEditsAreTakenVerbatim()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QPlainTextEdit *source = sourceEdit(&widget);
    QVERIFY(source);

    const auto replacement = QStringLiteral("client\nremote typed.example.org 1234 tcp\nsome-directive-of-mine 'odd  quoting'\n");
    tabs(&widget)->setCurrentIndex(2);
    source->setPlainText(replacement);

    QCOMPARE(storedProfileOf(widget.setting()), replacement);
}

void Openvpn3WidgetTest::aCommentTypedIntoTheSourceIsKept()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QPlainTextEdit *source = sourceEdit(&widget);
    QVERIFY(source);

    tabs(&widget)->setCurrentIndex(2);
    const QString typed = u"# a comment of mine\nclient\nremote typed.example.org 1234 tcp # here\n"_s;
    source->setPlainText(typed);

    // Taken exactly as typed.
    QCOMPARE(storedProfileOf(widget.setting()), typed);
    tabs(&widget)->setCurrentIndex(1);
    tabs(&widget)->setCurrentIndex(2);
    QCOMPARE(source->toPlainText(), typed);
    QCOMPARE(storedProfileOf(widget.setting()), typed);
}

void Openvpn3WidgetTest::theSourceShowsTheStoredDocumentVerbatim()
{
    // Whatever layout it is stored in, the Profile Source page shows the
    // stored document as it is, and saving without an edit stores it again
    // as it is.
    for (const auto &setting : {legacySetting(kCommented), secretSetting(u"1"_s, true, kCommented), upstreamSetting(u"0"_s, true, kCommented)}) {
        OpenVpn3SettingWidget widget(setting);
        QCOMPARE(sourceEdit(&widget)->toPlainText(), kCommented);
        QCOMPARE(widget.profileText(), kCommented);
        QCOMPARE(storedProfileOf(widget.setting()), kCommented);
    }
}

void Openvpn3WidgetTest::visitingThePagesNeverRewritesTheDocument()
{
    // A document the backend's import would normalize -- indentation, an
    // escaped trailing space, a scope naming a file that is not there, a
    // comment where a reference could be -- is not put through it by looking
    // at it: no page visit reimports it.
    const QString odd = u"# keep\n  client\nremote vpn1.example.org 1194 udp   ; note\nsetenv a value\\ \n# ca not-a-file.crt\n<connection>\nremote b.example.org\nca missing-scoped.crt\n</connection>\n<x-blob>\n  payload  \n</x-blob>"_s;
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, odd));
    for (int round = 0; round < 2; ++round) {
        for (int tab : {1, 2, 0, 2, 1, 0}) {
            tabs(&widget)->setCurrentIndex(tab);
            QCOMPARE(widget.profileText(), odd);
        }
    }
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    QCOMPARE(storedProfileOf(widget.setting()), odd);
    QVERIFY(!widget.hasProfileEdits());
}

void Openvpn3WidgetTest::tableAndFieldEditsKeepTheFormatting()
{
    // The editor end to end: the named fields, the server table and the
    // directive table all edit the one document, and the comments, blank
    // lines and inline comments of the stored profile survive every one of
    // those edits, the tab switches between them, the save, and a reload.
    const QString stored = u"# Provider notes\nclient\n\n; Primary server\nremote vpn1.example.org 1194 udp   # main\n# backup\nremote vpn2.example.org 443 tcp\ndev tun ; device\n\n"
                           u"<connection>\n# fallback\nremote fallback.example.org 1194 udp\n</connection>\nverb 3 # chatty\n# end"_s;
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, stored));

    // General page: a remote's port, and the device.
    QTableWidget *remotes = remotesTable(&widget);
    QCOMPARE(remotes->rowCount(), 2);
    remotes->item(0, 1)->setText(u"1195"_s);
    widget.findChild<QComboBox *>(u"openvpn3_device"_s)->setEditText(u"tap"_s);
    // ... and moving the backup server up, which takes its comment along.
    remotes->selectRow(1);
    widget.findChild<QPushButton *>(u"openvpn3_remote_up"_s)->click();

    // Directive table: an argument, a new row, a removal.
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);
    table->item(rowOfDirective(table, u"verb"_s), 2)->setText(u"4"_s);
    table->selectRow(rowOfDirective(table, u"client"_s));
    widget.findChild<QPushButton *>(u"openvpn3_directives_add"_s)->click();
    table->item(1, 1)->setText(u"nobind"_s);
    for (int row = 0; row < table->rowCount(); ++row) {
        QVERIFY(table->item(row, 0)->text() == u"Directive"_s || table->item(row, 0)->text() == u"Block"_s);
        QVERIFY(!table->item(row, 1)->text().startsWith(u'#'));
    }

    const QString expected = u"# Provider notes\nclient\nnobind\n\n# backup\nremote vpn2.example.org 443 tcp\n; Primary server\nremote vpn1.example.org 1195 udp   # main\ndev tap ; device\n\n"
                             u"<connection>\n# fallback\nremote fallback.example.org 1194 udp\n</connection>\nverb 4 # chatty\n# end"_s;
    tabs(&widget)->setCurrentIndex(2);
    QCOMPARE(sourceEdit(&widget)->toPlainText(), expected);
    tabs(&widget)->setCurrentIndex(0);
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    const QVariantMap map = widget.setting();
    QCOMPARE(storedProfileOf(map), expected);

    // Reloaded from what was saved, the same document, rows and all.
    OpenVpn3SettingWidget reloaded(settingFrom(dataOf(map), secretsOf(map)));
    QCOMPARE(reloaded.profileText(), expected);
    QCOMPARE(sourceEdit(&reloaded)->toPlainText(), expected);
    QCOMPARE(remotesTable(&reloaded)->item(0, 0)->text(), u"vpn2.example.org"_s);
}

void Openvpn3WidgetTest::aCrlfDocumentStaysCrlfThroughEdits()
{
    const QString stored = u"# w\r\nclient\r\nremote vpn1.example.org 1194 udp # main\r\n\r\ndev tun\r\n"_s;
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, stored));
    // The text box has no carriage returns to show; the document keeps them.
    QCOMPARE(sourceEdit(&widget)->toPlainText(), QString(stored).remove(u'\r'));
    tabs(&widget)->setCurrentIndex(2);
    tabs(&widget)->setCurrentIndex(0);
    QCOMPARE(storedProfileOf(widget.setting()), stored);

    widget.findChild<QComboBox *>(u"openvpn3_device"_s)->setEditText(u"tap"_s);
    QCOMPARE(storedProfileOf(widget.setting()), u"# w\r\nclient\r\nremote vpn1.example.org 1194 udp # main\r\n\r\ndev tap\r\n"_s);

    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(sourceEdit(&widget)->toPlainText() + u"verb 3\n"_s);
    tabs(&widget)->setCurrentIndex(0);
    QCOMPARE(storedProfileOf(widget.setting()), u"# w\r\nclient\r\nremote vpn1.example.org 1194 udp # main\r\n\r\ndev tap\r\nverb 3\r\n"_s);
}

void Openvpn3WidgetTest::anUpstreamConnectionIsSavedInTheUpstreamLayout()
{
    for (const QString &flags : {u"0"_s, u"1"_s}) {
        OpenVpn3SettingWidget widget(upstreamSetting(flags));
        QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
        // Its storage is what it says, offered without the public choice.
        QCOMPARE(storageCombo(&widget)->currentData().toInt(), flags.toInt());
        QCOMPARE(storageCombo(&widget)->findData(-1), -1);

        const QVariantMap map = widget.setting();
        QCOMPARE(dataOf(map).value(u"profile-flags"_s), flags);
        QCOMPARE(dataOf(map).value(u"remote"_s), u"vpn1.example.org"_s);
        QCOMPARE(dataOf(map).value(u"x-other"_s), u"keep"_s);
        QVERIFY(!dataOf(map).contains(u"profile-storage"_s));
        QVERIFY(!dataOf(map).contains(u"profile"_s));
        QCOMPARE(secretsOf(map).value(u"profile"_s), encoded(kProfile));
    }
}

void Openvpn3WidgetTest::aMarkerConnectionIsMovedToTheUpstreamLayout_data()
{
    QTest::addColumn<NMStringMap>("data");
    QTest::addColumn<QString>("flags");
    QTest::newRow("wallet") << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}} << u"1"_s;
    QTest::newRow("system") << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"0"_s}} << u"0"_s;
    // NetworkManager reads absent flags as system-owned, and so is it saved.
    QTest::newRow("no flags") << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"password-flags"_s, u"1"_s}} << u"0"_s;
    // And a stale public copy next to the secret is never what is saved.
    QTest::newRow("stale public copy") << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}, {u"profile"_s, encoded(u"client\nremote stale.example.org\n"_s)}}
                                       << u"1"_s;
}

void Openvpn3WidgetTest::aMarkerConnectionIsMovedToTheUpstreamLayout()
{
    QFETCH(NMStringMap, data);
    QFETCH(QString, flags);
    data.insert(u"username"_s, u"alice"_s);
    OpenVpn3SettingWidget widget(settingFrom(data, {{u"profile"_s, encoded(kProfile)}}));
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));

    const QVariantMap map = widget.setting();
    QCOMPARE(dataOf(map).value(u"profile-flags"_s), flags);
    QVERIFY(!dataOf(map).contains(u"profile-storage"_s));
    QVERIFY(!dataOf(map).contains(u"profile"_s));
    QCOMPARE(storedProfileOf(map), kProfile);
    QVERIFY(!storedProfileOf(map).contains(u"stale"_s));
}

void Openvpn3WidgetTest::staleOrUnknownLayoutsFailClosed_data()
{
    QTest::addColumn<NMStringMap>("data");
    QTest::addColumn<NMStringMap>("secrets");
    const NMStringMap secret{{u"profile"_s, encoded(kProfile)}};
    const NMStringMap none;
    const QString stale = encoded(u"client\nremote stale.example.org\n"_s);
    QTest::newRow("upstream, locked, stale copy") << NMStringMap{{u"profile-flags"_s, u"1"_s}, {u"profile"_s, stale}} << none;
    QTest::newRow("marker, locked, stale copy") << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"0"_s}, {u"profile"_s, stale}} << none;
    QTest::newRow("unknown marker") << NMStringMap{{u"profile-storage"_s, u"v9"_s}, {u"profile-flags"_s, u"0"_s}} << secret;
    QTest::newRow("upstream, flags 2") << NMStringMap{{u"profile-flags"_s, u"2"_s}} << secret;
    QTest::newRow("upstream, flags nonsense") << NMStringMap{{u"profile-flags"_s, u"x"_s}} << secret;
    QTest::newRow("marker, flags 4") << NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"4"_s}} << secret;
}

void Openvpn3WidgetTest::staleOrUnknownLayoutsFailClosed()
{
    QFETCH(NMStringMap, data);
    QFETCH(NMStringMap, secrets);
    OpenVpn3SettingWidget widget(settingFrom(data, secrets));

    // Nothing to save, and what comes back is the connection as it was.
    QVERIFY(!widget.isValid());
    QVERIFY(widget.storedProfileIsUnreadable());
    QVERIFY(!widget.profileText().contains(u"stale"_s));
    const QVariantMap map = widget.setting();
    QCOMPARE(dataOf(map), data);
    QCOMPARE(secretsOf(map), secrets);
}

void Openvpn3WidgetTest::lateSecretsCannotOverwriteAnEditedProfile()
{
    // A locked upstream connection whose secrets arrive after the user has
    // started over with a profile of their own: the reply is not allowed to
    // put the old one back.
    OpenVpn3SettingWidget widget(upstreamSetting(u"0"_s, false));
    QVERIFY(widget.storedProfileIsUnreadable());
    widget.loadSecrets(settingFrom({{u"profile-flags"_s, u"0"_s}}, {{u"profile"_s, encoded(kCommented)}}));
    QCOMPARE(widget.profileText(), kCommented);

    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(u"# mine\nclient\nremote mine.example.org\n"_s);
    widget.loadSecrets(settingFrom({{u"profile-flags"_s, u"0"_s}}, {{u"profile"_s, encoded(kProfile)}, {u"challenge-response"_s, u"123456"_s}}));
    QCOMPARE(storedProfileOf(widget.setting()), u"# mine\nclient\nremote mine.example.org\n"_s);
    // And a one-time code that came with it is never stored.
    QVERIFY(!secretsOf(widget.setting()).contains(u"challenge-response"_s));
}

void Openvpn3WidgetTest::connectionBlockCaveatsAreShown()
{
    const QString profile = u"client\nremote vpn1.example.org\n<connection>\nremote b.example.org\nca /etc/scoped-ca.crt\nhttp-proxy p.example.org 8080\n</connection>\n"_s;
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, profile));
    // A caveat, not a refusal: the scope is saved as written.
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    const QString status = widget.findChild<QLabel *>(u"openvpn3_status"_s)->text();
    QVERIFY2(status.contains(u"/etc/scoped-ca.crt"_s), qPrintable(status));
    QVERIFY2(status.contains(u"http-proxy"_s), qPrintable(status));
    QCOMPARE(storedProfileOf(widget.setting()), profile);
}

void Openvpn3WidgetTest::resolvingAReferenceTouchesOnlyThatEntry()
{
    OPENVPN3_REQUIRE_BACKEND();
    // A file reference typed into the source is embedded when the page is
    // left, through the backend, and nothing else in the document moves: not
    // the formatting, not a line the import would have stripped, not a scope.
    const QString ca = dataPath(u"pki/ca.crt"_s);
    const QString head = u"# typed\nclient\n  remote vpn1.example.org 1194 udp ; main\nsetenv a value\\ \n"_s;
    const QString tail = u"\n<connection>\nremote b.example.org\nca scoped.crt\n</connection>\nverb 3"_s;
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s));
    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(head + u"ca "_s + ca + u" # company\n"_s + tail);
    tabs(&widget)->setCurrentIndex(1);

    QFile file(ca);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString expected = head + u"# company\n<ca>\n"_s + QString::fromUtf8(file.readAll()) + u"</ca>\n"_s + tail;
    QCOMPARE(widget.profileText(), expected);
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    QCOMPARE(storedProfileOf(widget.setting()), expected);
}

void Openvpn3WidgetTest::editingAServerKeepsWhatOpenvpn3ReadsFromTheLine()
{
    // A hash glued to the host is a comment for openvpn3; editing the port on
    // the General page must not make it part of the host name.
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, u"client\nremote vpn1.example.org#primary 1194 udp\n"_s));
    QTableWidget *remotes = remotesTable(&widget);
    QCOMPARE(remotes->item(0, 0)->text(), u"vpn1.example.org"_s);
    remotes->item(0, 1)->setText(u"1195"_s);
    QCOMPARE(storedProfileOf(widget.setting()), u"client\nremote vpn1.example.org 1195 #primary 1194 udp\n"_s);
    QCOMPARE(Openvpn3Profile::fromText(storedProfileOf(widget.setting())).remoteHosts(), QStringList({u"vpn1.example.org"_s}));
}

void Openvpn3WidgetTest::aMissingFileThatAppearsIsPickedUpWithoutAnEdit()
{
    OPENVPN3_REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"late-ca.pem"_s);
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s));
    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(u"client\nremote vpn1.example.org\nca "_s + path + u"\n"_s);
    QVERIFY(!widget.isValid());
    QVERIFY2(widget.blockingProblem().contains(path), qPrintable(widget.blockingProblem()));

    // The file is put there; nothing in the profile changes, and the next
    // look at it finds the file rather than the old failure.
    QVERIFY(QFile::copy(dataPath(u"pki/ca.crt"_s), path));
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    tabs(&widget)->setCurrentIndex(0);
    QVERIFY(widget.profileText().contains(u"<ca>\n"_s));
}

void Openvpn3WidgetTest::aChangedOrDeletedFileIsReadAgainOnSave()
{
    OPENVPN3_REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"client.key"_s);
    const auto write = [&path](const QByteArray &contents) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
    };
    QVERIFY(write("-----BEGIN PRIVATE KEY-----\nOLD\n-----END PRIVATE KEY-----\n"));
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s));
    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(u"client\nremote vpn1.example.org\nkey "_s + path + u"\n"_s);
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));

    // Replaced while the page shows the reference: the save reads it now.
    QVERIFY(write("-----BEGIN PRIVATE KEY-----\nNEW\n-----END PRIVATE KEY-----\n"));
    QVERIFY(widget.isValid());
    QVERIFY(storedProfileOf(widget.setting()).contains(u"\nNEW\n"_s));
    QVERIFY(!storedProfileOf(widget.setting()).contains(u"OLD"_s));

    // Deleted: not saveable, and the profile is not changed.
    QVERIFY(QFile::remove(path));
    QVERIFY(!widget.isValid());
    QVERIFY2(widget.blockingProblem().contains(path), qPrintable(widget.blockingProblem()));
    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
}

void Openvpn3WidgetTest::textInsideAnOpaqueBlockIsNeverResolved()
{
    // An indented closing tag is payload for openvpn3, so the reference after
    // it is too; saving reads no file and changes nothing.
    const QString text = u"client\nremote vpn1.example.org\n<x-notes>\n </x-notes>\nca /nonexistent/ca.pem\n</x-notes>\n# ca /nonexistent/other.pem\n"_s;
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, text));
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    for (int tab : {1, 2, 0}) {
        tabs(&widget)->setCurrentIndex(tab);
    }
    QCOMPARE(storedProfileOf(widget.setting()), text);
}

void Openvpn3WidgetTest::visitingThePagesNeverResolvesWhatWasLoaded()
{
    OPENVPN3_REQUIRE_BACKEND();
    // A stored profile that still points outside itself -- written by
    // something else -- is shown as it is stored however often the pages are
    // visited; only saving makes it self-contained.
    const QString stored = u"client\nremote vpn1.example.org\nca "_s + dataPath(u"pki/ca.crt"_s)
        + u" # company CA\n<auth-user-pass>\nalice\nstored-pw\n</auth-user-pass>\n"_s;
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, stored));
    for (int tab : {2, 1, 0, 2, 0, 1, 2}) {
        tabs(&widget)->setCurrentIndex(tab);
        QCOMPARE(widget.profileText(), stored);
    }
    QCOMPARE(sourceEdit(&widget)->toPlainText(), stored);
    QVERIFY(!widget.hasProfileEdits());

    // A reference typed in afterwards is resolved on leaving the page, and
    // the loaded ones still are not.
    const QString typed = stored + u"cert "_s + dataPath(u"pki/client.crt"_s) + u"\n"_s;
    sourceEdit(&widget)->setPlainText(typed);
    tabs(&widget)->setCurrentIndex(0);
    QVERIFY(widget.profileText().startsWith(stored));
    QVERIFY(widget.profileText().contains(u"<cert>\n"_s));
    QVERIFY(!widget.profileText().contains(u"client.crt"_s));

    // Saving makes the whole thing self-contained.
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    const QVariantMap map = widget.setting();
    QVERIFY(!storedProfileOf(map).contains(u"ca.crt"_s));
    QVERIFY(storedProfileOf(map).contains(u"# company CA\n<ca>\n"_s));
    QVERIFY(!storedProfileOf(map).contains(u"stored-pw"_s));
    QCOMPARE(dataOf(map).value(u"username"_s), u"alice"_s);
    QCOMPARE(secretsOf(map).value(u"password"_s), u"stored-pw"_s);
}

void Openvpn3WidgetTest::removingARowKeepsItsCommentThroughSaveAndReload()
{
    const QString stored = u"client\n# servers\nremote vpn1.example.org # retain this note\nremote vpn2.example.org\n"_s;
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, stored));
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);
    table->selectRow(rowOfDirective(table, u"remote"_s));
    widget.findChild<QPushButton *>(u"openvpn3_directives_remove"_s)->click();

    const QString expected = u"client\n# servers\n# retain this note\nremote vpn2.example.org\n"_s;
    tabs(&widget)->setCurrentIndex(2);
    QCOMPARE(sourceEdit(&widget)->toPlainText(), expected);
    tabs(&widget)->setCurrentIndex(0);
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    const QVariantMap map = widget.setting();
    QCOMPARE(storedProfileOf(map), expected);
    OpenVpn3SettingWidget reloaded(settingFrom(dataOf(map), secretsOf(map)));
    QCOMPARE(reloaded.profileText(), expected);

    // The same through the server table on the General page.
    OpenVpn3SettingWidget general(upstreamSetting(u"1"_s, true, stored));
    remotesTable(&general)->selectRow(0);
    general.findChild<QPushButton *>(u"openvpn3_remote_remove"_s)->click();
    QCOMPARE(storedProfileOf(general.setting()), expected);
}

void Openvpn3WidgetTest::credentialsAreTheSameWithOrWithoutATabSwitch()
{
    OPENVPN3_REQUIRE_BACKEND();
    // A stored profile with inline credentials, and a second set typed in
    // after them: the backend takes the later one, and so does the save,
    // whether or not the pages were switched in between.
    const QString stored = u"client\nremote vpn1.example.org\n<auth-user-pass>\nalice\nold-password\n</auth-user-pass>\n"_s;
    const QString typed = stored + u"<auth-user-pass>\nbob\nnew-password\n</auth-user-pass>\n"_s;
    QList<QPair<QString, QString>> saved;
    for (const bool switchTabs : {false, true}) {
        OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s, true, stored));
        tabs(&widget)->setCurrentIndex(2);
        sourceEdit(&widget)->setPlainText(typed);
        if (switchTabs) {
            tabs(&widget)->setCurrentIndex(0);
        }
        QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
        const QVariantMap map = widget.setting();
        saved.append({dataOf(map).value(u"username"_s), secretsOf(map).value(u"password"_s)});
        QVERIFY(!storedProfileOf(map).contains(u"password"_s));
    }
    QCOMPARE(saved.at(0), (QPair<QString, QString>{u"bob"_s, u"new-password"_s}));
    QCOMPARE(saved.at(1), saved.at(0));
}

void Openvpn3WidgetTest::aRelativePathIsExplained()
{
    OpenVpn3SettingWidget widget(upstreamSetting(u"1"_s));
    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(u"client\nremote vpn1.example.org\nkey keys/client.key\n"_s);
    QVERIFY(!widget.isValid());
    QVERIFY2(widget.blockingProblem().contains(u"keys/client.key"_s), qPrintable(widget.blockingProblem()));
    QVERIFY2(widget.blockingProblem().contains(u"relative"_s), qPrintable(widget.blockingProblem()));
    // Nothing was thrown away meanwhile.
    tabs(&widget)->setCurrentIndex(0);
    QCOMPARE(widget.profileText(), u"client\nremote vpn1.example.org\nkey keys/client.key\n"_s);
}

void Openvpn3WidgetTest::sourceEditsReachTheNamedFields()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QPlainTextEdit *source = sourceEdit(&widget);

    tabs(&widget)->setCurrentIndex(2);
    source->setPlainText(u"client\nremote typed.example.org 1234 tcp\n"_s);
    tabs(&widget)->setCurrentIndex(0);

    QTableWidget *table = remotesTable(&widget);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->text(), u"typed.example.org"_s);
    QCOMPARE(table->item(0, 1)->text(), u"1234"_s);
    QCOMPARE(table->item(0, 2)->text(), u"tcp"_s);
}

void Openvpn3WidgetTest::aLockedProfileIsNeverOverwritten()
{
    auto setting = secretSetting(u"1"_s, /*withSecret=*/false);
    OpenVpn3SettingWidget widget(setting);

    QVERIFY(widget.storedProfileIsUnreadable());
    QVERIFY(!widget.isValid());

    // Even if something asked for the setting anyway, the stored profile is
    // left exactly as it is rather than replaced with an empty one.
    const QVariantMap map = widget.setting();
    QCOMPARE(dataOf(map).value(u"profile-storage"_s), u"secret"_s);
    QVERIFY(!secretsOf(map).contains(u"profile"_s));
    QVERIFY(!dataOf(map).contains(u"profile"_s));
}

void Openvpn3WidgetTest::anUnknownStorageLayoutBlocksSaving()
{
    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    setting->setData({{u"profile-storage"_s, u"v2-whatever"_s}, {u"profile"_s, encoded(kProfile)}});
    OpenVpn3SettingWidget widget(setting);

    QVERIFY(widget.storedProfileIsUnreadable());
    QVERIFY(!widget.isValid());
    // The stale public copy is not shown as if it were the profile.
    QVERIFY(widget.profileText().isEmpty());
}

void Openvpn3WidgetTest::unusableProfileFlagsBlockSaving()
{
    OpenVpn3SettingWidget widget(secretSetting(u"2"_s));

    QVERIFY(widget.storedProfileIsUnreadable());
    QVERIFY(!widget.isValid());
}

void Openvpn3WidgetTest::loadSecretsFillsInALockedProfile()
{
    OpenVpn3SettingWidget widget(secretSetting(u"1"_s, /*withSecret=*/false));
    QVERIFY(!widget.isValid());

    auto reply = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    reply->setSecrets({{u"profile"_s, encoded(kProfile)}});
    widget.loadSecrets(reply);

    QVERIFY(!widget.storedProfileIsUnreadable());
    QVERIFY(widget.isValid());
    QCOMPARE(widget.profileText(), kProfile);
    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
}

void Openvpn3WidgetTest::loadSecretsWithoutTheProfileDoesNotBlankIt()
{
    OpenVpn3SettingWidget widget(secretSetting());
    QCOMPARE(widget.profileText(), kProfile);

    // An agent that only had the password: the profile must survive.
    auto reply = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    reply->setSecrets({{u"password"_s, u"pw"_s}});
    widget.loadSecrets(reply);

    QCOMPARE(widget.profileText(), kProfile);
    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
    QVERIFY(widget.isValid());
}

void Openvpn3WidgetTest::aProfileWithoutARemoteIsNotValid()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\ndev tun\n"_s));
    QVERIFY(!widget.isValid());
}

void Openvpn3WidgetTest::aRemoteInsideAConnectionBlockCounts()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\n<connection>\nremote only.example.org 1194\n</connection>\n"_s));
    QVERIFY(widget.isValid());
}

void Openvpn3WidgetTest::oneTimeCodesAreMarkedAsNeverStored()
{
    OpenVpn3SettingWidget widget(legacySetting());

    QCOMPARE(dataOf(widget.setting()).value(u"challenge-response-flags"_s), QString::number(NetworkManager::Setting::NotSaved));
}

void Openvpn3WidgetTest::nothingSensitiveEverReachesTheDataMap()
{
    const auto withKey = QStringLiteral(
        "client\nremote a.example.org\n<key>\n-----BEGIN PRIVATE KEY-----\nVERYSECRET\n-----END PRIVATE KEY-----\n</key>\n");
    OpenVpn3SettingWidget widget(legacySetting(withKey));

    QComboBox *combo = storageCombo(&widget);
    combo->setCurrentIndex(combo->findData(static_cast<int>(NetworkManager::Setting::AgentOwned)));

    const NMStringMap data = dataOf(widget.setting());
    for (const QString &value : data.values()) {
        QVERIFY(!value.contains(u"VERYSECRET"_s));
        QVERIFY(!value.contains(encoded(withKey)));
    }
}

// -- reimport ----------------------------------------------------------------

void Openvpn3WidgetTest::reimportReplacesOnlyTheVpnConfiguration()
{
    OPENVPN3_REQUIRE_BACKEND();
    OpenVpn3SettingWidget widget(legacySetting());
    QCOMPARE(dataOf(widget.setting()).value(u"username"_s), u"alice"_s);

    QString error;
    QVERIFY2(widget.importProfile(dataPath(u"office.ovpn"_s), &error), qPrintable(error));

    const QVariantMap map = widget.setting();
    // The profile is the imported one, self-contained.
    const QString stored = storedProfileOf(map);
    QVERIFY(stored.contains(u"vpn1.example.org"_s));
    QVERIFY(!stored.contains(u"pki/ca.crt"_s));
    QVERIFY(stored.contains(u"BEGIN CERTIFICATE"_s));
    // Credentials come from the file; the profile moves to the wallet.
    QCOMPARE(dataOf(map).value(u"username"_s), u"alice"_s);
    QCOMPARE(secretsOf(map).value(u"password"_s), u"correct-horse-battery-staple"_s);
    QCOMPARE(dataOf(map).value(u"profile-flags"_s), u"1"_s);
    QVERIFY(!dataOf(map).contains(u"profile"_s));
    // Nothing outside the vpn setting is in this widget's gift to change: it
    // only ever returns properties of the vpn setting itself, so the
    // connection's identity, addresses, routes and permissions are untouched.
    const QSet<QString> vpnProperties{u"service-type"_s, u"data"_s, u"secrets"_s, u"user-name"_s, u"persistent"_s, u"timeout"_s};
    const QList<QString> returned = map.keys();
    for (const QString &key : returned) {
        QVERIFY2(vpnProperties.contains(key), qPrintable(key));
    }
    QVERIFY(widget.isValid());
}

void Openvpn3WidgetTest::aFailedReimportChangesNothing()
{
    OpenVpn3SettingWidget widget(legacySetting());
    const QVariantMap before = widget.setting();

    QString error;
    QVERIFY(!widget.importProfile(u"/nonexistent/nowhere.ovpn"_s, &error));
    QVERIFY(!error.isEmpty());

    QCOMPARE(widget.setting(), before);
    QCOMPARE(widget.profileText(), kProfile);
}

void Openvpn3WidgetTest::reimportKeepsAnExplicitSystemStorageChoice()
{
    OPENVPN3_REQUIRE_BACKEND();
    OpenVpn3SettingWidget widget(secretSetting(u"0"_s));

    QVERIFY(widget.importProfile(dataPath(u"office.ovpn"_s)));

    QCOMPARE(dataOf(widget.setting()).value(u"profile-flags"_s), u"0"_s);
    QCOMPARE(storageCombo(&widget)->currentData().toInt(), static_cast<int>(NetworkManager::Setting::None));
}

void Openvpn3WidgetTest::editingMarksTheProfileSoAReimportCanWarn()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QVERIFY(!widget.hasProfileEdits());

    portEdit(&widget)->setText(u"1195"_s);
    Q_EMIT portEdit(&widget)->editingFinished();

    QVERIFY(widget.hasProfileEdits());
}

void Openvpn3WidgetTest::vpnPropertiesThisPageDoesNotShowSurviveASave()
{
    OPENVPN3_REQUIRE_BACKEND();
    // The editor replaces the whole vpn setting, so a timeout set with nmcli
    // must not be reset to the default just because this page never shows it.
    auto setting = legacySetting();
    setting->setTimeout(180);
    setting->setPersistent(true);
    setting->setUsername(u"vpn-owner"_s);
    OpenVpn3SettingWidget widget(setting);

    const QVariantMap map = widget.setting();
    QCOMPARE(map.value(u"timeout"_s).toUInt(), 180u);
    QCOMPARE(map.value(u"persistent"_s).toBool(), true);
    QCOMPARE(map.value(u"user-name"_s).toString(), u"vpn-owner"_s);
    QVERIFY(widget.importProfile(dataPath(u"office.ovpn"_s)));
    const auto reimported = widget.setting();
    QCOMPARE(reimported.value(u"timeout"_s).toUInt(), 180u);
    QCOMPARE(reimported.value(u"persistent"_s).toBool(), true);
    QCOMPARE(reimported.value(u"user-name"_s).toString(), u"vpn-owner"_s);
}

void Openvpn3WidgetTest::storedSecretsAreNotShownInClear()
{
    // PasswordField does not mask by default; the reveal button is how a
    // password is meant to be read, not the page being open.
    OpenVpn3SettingWidget widget(legacySetting());

    for (const QString &name : {u"openvpn3_password"_s, u"openvpn3_certpass"_s}) {
        auto *field = widget.findChild<PasswordField *>(name);
        QVERIFY2(field, qPrintable(name));
        auto *line = field->findChild<QLineEdit *>();
        QVERIFY(line);
        QCOMPARE(line->echoMode(), QLineEdit::Password);
    }
}

// -- the generic directive table ---------------------------------------------

void Openvpn3WidgetTest::directiveTableEditsReachTheStoredProfile()
{
    OpenVpn3SettingWidget widget(legacySetting());
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);
    QVERIFY(table);
    QCOMPARE(table->rowCount(), Openvpn3Profile::fromText(kProfile).count());

    // Change the arguments of the directive the named fields know nothing of.
    const int row = rowOfDirective(table, u"some-directive-we-have-never-heard-of"_s);
    QVERIFY(row >= 0);
    table->item(row, 2)->setText(u"4 5 \"six and seven\""_s);

    const QString stored = storedProfileOf(widget.setting());
    QVERIFY(stored.contains(u"some-directive-we-have-never-heard-of 4 5 \"six and seven\"\n"_s));
}

void Openvpn3WidgetTest::directiveTableKeepsEntriesTheEditorDoesNotUnderstand()
{
    OpenVpn3SettingWidget widget(legacySetting());
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);

    // Every entry of the profile is a row, and every row is an entry: the
    // formatting is not of a kind the table has.
    QStringList kinds;
    for (int row = 0; row < table->rowCount(); ++row) {
        kinds.append(table->item(row, 0)->text());
    }
    QVERIFY(!kinds.contains(u"Comment"_s));
    QVERIFY(!kinds.contains(u"Blank line"_s));
    QVERIFY(kinds.contains(u"Block"_s));
    QVERIFY(kinds.contains(u"Directive"_s));
    QCOMPARE(QSet<QString>(kinds.cbegin(), kinds.cend()), QSet<QString>({u"Directive"_s, u"Block"_s}));

    // Switching away and back changes nothing.
    tabs(&widget)->setCurrentIndex(0);
    tabs(&widget)->setCurrentIndex(1);
    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
}

void Openvpn3WidgetTest::commentsInAStoredProfileAreKeptButNotShownAsRows()
{
    // A profile the backend imported keeps its comments and blank lines.
    // The table shows rows for none of them, and saving writes all of them
    // back.
    OpenVpn3SettingWidget widget(legacySetting(kCommented));
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);

    QCOMPARE(table->rowCount(), 4); // client, remote, setenv, <ca>
    for (int row = 0; row < table->rowCount(); ++row) {
        QVERIFY(table->item(row, 0)->text() != u"Comment"_s);
        QVERIFY(table->item(row, 0)->text() != u"Blank line"_s);
    }
    QVERIFY(rowOfDirective(table, u"client"_s) >= 0);
    QVERIFY(rowOfDirective(table, u"ca"_s) >= 0);

    QCOMPARE(widget.profileText(), kCommented);
    QCOMPARE(storedProfileOf(widget.setting()), kCommented);
    QCOMPARE(Openvpn3Profile::fromText(widget.profileText()).optionsText(), kCommentedKept);

    // And the editor still reads the profile the same way.
    QTableWidget *remotes = remotesTable(&widget);
    QCOMPARE(remotes->rowCount(), 1);
    QCOMPARE(remotes->item(0, 0)->text(), u"vpn1.example.org"_s);
}

void Openvpn3WidgetTest::aCommentTypedIntoAScopeBodyIsSaved()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\n<connection>\nremote a.example.org 1194 udp\n</connection>\n"_s));
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);

    const int row = rowOfDirective(table, u"connection"_s);
    QVERIFY(row >= 0);
    table->selectRow(row);

    auto *body = widget.findChild<QPlainTextEdit *>(u"openvpn3_directives_body"_s);
    QVERIFY(body);
    body->setPlainText(u"# mine\nremote b.example.org 443 tcp # here\n"_s);

    const auto expected = u"client\n<connection>\n# mine\nremote b.example.org 443 tcp # here\n</connection>\n"_s;
    QCOMPARE(widget.profileText(), expected);
    QCOMPARE(storedProfileOf(widget.setting()), expected);
    tabs(&widget)->setCurrentIndex(2);
    QCOMPARE(sourceEdit(&widget)->toPlainText(), expected);
}

void Openvpn3WidgetTest::blankLinesInAStoredProfileAreKeptButNotShownAsRows()
{
    const auto stored = u"\nclient\n\nremote vpn1.example.org 1194 udp\n   \n<ca>\n\nPEM\n\n</ca>\n\n"_s;
    OpenVpn3SettingWidget widget(legacySetting(stored));
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);

    QCOMPARE(table->rowCount(), 3); // client, remote, <ca>
    for (int row = 0; row < table->rowCount(); ++row) {
        QVERIFY(table->item(row, 0)->text() != u"Blank line"_s);
        QVERIFY(!table->item(row, 1)->text().isEmpty());
    }
    QCOMPARE(widget.profileText(), stored);
    QCOMPARE(storedProfileOf(widget.setting()), stored);

    // Visiting every page and coming back is not an edit, and saving again
    // changes nothing more.
    for (int tab = 0; tab < tabs(&widget)->count(); ++tab) {
        tabs(&widget)->setCurrentIndex(tab);
    }
    QCOMPARE(storedProfileOf(widget.setting()), stored);
    OpenVpn3SettingWidget again(legacySetting(storedProfileOf(widget.setting())));
    QCOMPARE(storedProfileOf(again.setting()), stored);
}

void Openvpn3WidgetTest::aBlankLineTypedIntoTheSourceIsSaved()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QPlainTextEdit *source = sourceEdit(&widget);
    QVERIFY(source);

    tabs(&widget)->setCurrentIndex(2);
    source->setPlainText(u"\nclient\n\nremote typed.example.org 1234 tcp\n  \n"_s);

    QCOMPARE(storedProfileOf(widget.setting()), u"\nclient\n\nremote typed.example.org 1234 tcp\n  \n"_s);
    tabs(&widget)->setCurrentIndex(0);
    QCOMPARE(source->toPlainText(), u"\nclient\n\nremote typed.example.org 1234 tcp\n  \n"_s);
}

void Openvpn3WidgetTest::aBlankLineTypedIntoAScopeBodyIsSaved()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\n<connection>\nremote a.example.org 1194 udp\n</connection>\n"_s));
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);

    const int row = rowOfDirective(table, u"connection"_s);
    QVERIFY(row >= 0);
    table->selectRow(row);

    auto *body = widget.findChild<QPlainTextEdit *>(u"openvpn3_directives_body"_s);
    QVERIFY(body);
    body->setPlainText(u"\nremote b.example.org 443 tcp\n\n"_s);

    const auto expected = u"client\n<connection>\n\nremote b.example.org 443 tcp\n\n</connection>\n"_s;
    QCOMPARE(widget.profileText(), expected);
    QCOMPARE(storedProfileOf(widget.setting()), expected);
}

void Openvpn3WidgetTest::aBlockBodyCanBeEditedInTheTable()
{
    OpenVpn3SettingWidget widget(legacySetting());
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);

    const int row = rowOfDirective(table, u"ca"_s);
    QVERIFY(row >= 0);
    table->selectRow(row);

    auto *body = widget.findChild<QPlainTextEdit *>(u"openvpn3_directives_body"_s);
    QVERIFY(body);
    QVERIFY(body->isEnabled());
    QVERIFY(body->toPlainText().contains(u"BEGIN CERTIFICATE"_s));

    body->setPlainText(u"REPLACED\n"_s);
    const QString stored = storedProfileOf(widget.setting());
    QVERIFY(stored.contains(u"<ca>\nREPLACED\n</ca>\n"_s));
    QVERIFY(!stored.contains(u"BEGIN CERTIFICATE"_s));
}

void Openvpn3WidgetTest::removingARemoteDoesNotMoveAnothersExtraArguments()
{
    // Arguments past host/port/transport belong to the row they were on, not
    // to the position it happened to occupy.
    OpenVpn3SettingWidget widget(
        legacySetting(u"client\nremote a.example.org 1194 udp keepalive-hack\nremote b.example.org 443 tcp\n"_s));
    QTableWidget *table = remotesTable(&widget);
    QCOMPARE(table->rowCount(), 2);

    table->removeRow(0);
    table->setItem(0, 0, new QTableWidgetItem(u"b.example.org"_s));

    const QString stored = storedProfileOf(widget.setting());
    QCOMPARE(stored, u"client\nremote b.example.org 443 tcp\n"_s);
}

void Openvpn3WidgetTest::reorderingRemotesCarriesTheirExtraArguments()
{
    OpenVpn3SettingWidget widget(
        legacySetting(u"client\nremote a.example.org 1194 udp keepalive-hack\nremote b.example.org 443 tcp\n"_s));
    QTableWidget *table = remotesTable(&widget);

    auto *moveDown = widget.findChild<QPushButton *>(u"openvpn3_remote_down"_s);
    QVERIFY(moveDown);
    table->selectRow(0);
    moveDown->click();

    const QString stored = storedProfileOf(widget.setting());
    QCOMPARE(stored, u"client\nremote b.example.org 443 tcp\nremote a.example.org 1194 udp keepalive-hack\n"_s);
}

void Openvpn3WidgetTest::aProfileWithoutKeyMaterialStoresNoPassphraseKeys()
{
    // Writing cert-pass-flags on a connection that has no private key would
    // make the secrets prompt ask for a passphrase that nothing ever uses.
    OpenVpn3SettingWidget widget(legacySetting());
    const QVariantMap map = widget.setting();

    QVERIFY(!dataOf(map).contains(u"cert-pass-flags"_s));
    QVERIFY(!secretsOf(map).contains(u"cert-pass"_s));
}

void Openvpn3WidgetTest::aProfileWithAnEncryptedKeyStoresThePassphraseFlags()
{
    const auto encrypted = QStringLiteral(
        "client\nremote a.example.org\n<key>\n-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n-----END ENCRYPTED PRIVATE KEY-----\n</key>\n");
    OpenVpn3SettingWidget widget(legacySetting(encrypted));

    QCOMPARE(dataOf(widget.setting()).value(u"cert-pass-flags"_s), QString::number(NetworkManager::Setting::AgentOwned));
}

// -- what the connection editor is told about an edit -------------------------

void Openvpn3WidgetTest::aSourceEditEnablesSaving()
{
    // The connection editor enables Save on settingChanged(). Reporting only
    // that the page is still valid leaves the user typing a whole profile
    // into a dialog with a greyed-out Save button.
    OpenVpn3SettingWidget widget(legacySetting());
    QSignalSpy changed(&widget, &SettingWidget::settingChanged);

    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(u"client\nremote typed.example.org 1234 tcp\n"_s);

    QVERIFY(!changed.isEmpty());
}

void Openvpn3WidgetTest::aDirectiveTableEditEnablesSaving()
{
    OpenVpn3SettingWidget widget(legacySetting());
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);
    const int row = rowOfDirective(table, u"some-directive-we-have-never-heard-of"_s);
    QVERIFY(row >= 0);

    QSignalSpy changed(&widget, &SettingWidget::settingChanged);
    table->item(row, 2)->setText(u"4 5 6"_s);

    QVERIFY(!changed.isEmpty());
}

void Openvpn3WidgetTest::aBlockBodyEditEnablesSaving()
{
    OpenVpn3SettingWidget widget(legacySetting());
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);
    table->selectRow(rowOfDirective(table, u"ca"_s));
    auto *body = widget.findChild<QPlainTextEdit *>(u"openvpn3_directives_body"_s);
    QVERIFY(body);

    QSignalSpy changed(&widget, &SettingWidget::settingChanged);
    body->setPlainText(u"REPLACED\n"_s);

    QVERIFY(!changed.isEmpty());
}

void Openvpn3WidgetTest::aCredentialEditCountsAsAnUnsavedChange()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QVERIFY(!widget.hasUnsavedEdits());

    QSignalSpy changed(&widget, &SettingWidget::settingChanged);
    widget.findChild<PasswordField *>(u"openvpn3_password"_s)->setText(u"typed"_s);

    QVERIFY(!changed.isEmpty());
    QVERIFY(widget.hasUnsavedEdits());
    // The profile itself is untouched, and an import would still throw the
    // typed password away, so the confirmation has to know about both.
    QVERIFY(!widget.hasProfileEdits());
}

void Openvpn3WidgetTest::aStorageChoiceCountsAsAnUnsavedChange()
{
    OpenVpn3SettingWidget widget(secretSetting());
    QVERIFY(!widget.hasUnsavedEdits());

    QComboBox *combo = storageCombo(&widget);
    QSignalSpy changed(&widget, &SettingWidget::settingChanged);
    combo->setCurrentIndex(combo->findData(static_cast<int>(NetworkManager::Setting::None)));

    QVERIFY(!changed.isEmpty());
    QVERIFY(widget.hasUnsavedEdits());
}

// -- the profile source page --------------------------------------------------

void Openvpn3WidgetTest::visitingEveryTabChangesNothingInACrlfProfile()
{
    // A QPlainTextEdit holds text without carriage returns in it, so reading
    // a profile back out of one rewrites every line of a profile written on
    // Windows. Opening a page is not editing it.
    const auto crlf = QStringLiteral("client\r\ndev tun\r\nremote vpn.example.org 1194 udp\r\n<ca>\r\nPEM\r\n</ca>\r\n");
    OpenVpn3SettingWidget widget(legacySetting(crlf));

    for (int tab = 0; tab < tabs(&widget)->count(); ++tab) {
        tabs(&widget)->setCurrentIndex(tab);
    }
    tabs(&widget)->setCurrentIndex(0);

    QCOMPARE(widget.profileText(), crlf);
    QCOMPARE(storedProfileOf(widget.setting()), crlf);
    QVERIFY(!widget.hasProfileEdits());
}

void Openvpn3WidgetTest::anEditedCrlfProfileKeepsItsLineEndings()
{
    const auto crlf = QStringLiteral("client\r\nremote vpn.example.org 1194 udp\r\n");
    OpenVpn3SettingWidget widget(legacySetting(crlf));
    QPlainTextEdit *source = sourceEdit(&widget);

    tabs(&widget)->setCurrentIndex(2);
    // What the user sees and edits has no carriage returns in it; the
    // document they are editing does, and still does afterwards.
    source->setPlainText(u"client\nremote vpn.example.org 1194 udp\ncomp-lzo no\n"_s);

    QCOMPARE(storedProfileOf(widget.setting()), u"client\r\nremote vpn.example.org 1194 udp\r\ncomp-lzo no\r\n"_s);
}

// -- servers -------------------------------------------------------------------

void Openvpn3WidgetTest::aBlankServerRowIsNotSomethingToSave()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\ndev tun\n"_s));
    QVERIFY(!widget.isValid());

    auto *add = widget.findChild<QPushButton *>(u"openvpn3_remote_add"_s);
    QVERIFY(add);
    add->click();

    // There is a remote directive now, and it names no server, which is not
    // a connection that could ever come up.
    QCOMPARE(remotesTable(&widget)->rowCount(), 1);
    QVERIFY(!widget.isValid());
    QVERIFY(!widget.blockingProblem().isEmpty());

    remotesTable(&widget)->item(0, 0)->setText(u"typed.example.org"_s);
    QVERIFY(widget.isValid());
}

void Openvpn3WidgetTest::removingAServerLeavesTheOtherDirectivesWhereTheyWere()
{
    // Rows are entries of the document, not positions in it: removing the
    // first one must not rewrite the second in its place and delete the last
    // entry, which would move the surviving remote across the directives
    // that were between them.
    OpenVpn3SettingWidget widget(legacySetting(
        u"client\nremote a.example.org 1194 udp\nkeepalive 10 60\nremote b.example.org 443 tcp extra-arg\nverb 3\n"_s));
    QTableWidget *table = remotesTable(&widget);
    QCOMPARE(table->rowCount(), 2);

    table->selectRow(0);
    widget.findChild<QPushButton *>(u"openvpn3_remote_remove"_s)->click();

    QCOMPARE(storedProfileOf(widget.setting()), u"client\nkeepalive 10 60\nremote b.example.org 443 tcp extra-arg\nverb 3\n"_s);
}

void Openvpn3WidgetTest::reorderingServersMovesOnlyTheServers()
{
    OpenVpn3SettingWidget widget(
        legacySetting(u"client\nremote a.example.org 1194 udp extra-a\nkeepalive 10 60\nremote b.example.org 443 tcp\n"_s));
    QTableWidget *table = remotesTable(&widget);

    table->selectRow(0);
    widget.findChild<QPushButton *>(u"openvpn3_remote_down"_s)->click();

    // The two servers change places; the directive between them does not move
    // and the trailing argument stays with the server it belongs to.
    QCOMPARE(storedProfileOf(widget.setting()),
             u"client\nremote b.example.org 443 tcp\nkeepalive 10 60\nremote a.example.org 1194 udp extra-a\n"_s);
}

// -- scoped directives ---------------------------------------------------------

void Openvpn3WidgetTest::credentialsForAProfileThatAsksInsideAConnectionBlockSurvive()
{
    // openvpn3 itself ignores an auth-user-pass inside a <connection> block,
    // but the backend's service does not: it refuses to connect without a
    // username (service.py) and asks for the password. Looking only at the
    // top level would throw the stored credentials away on the next save,
    // and the connection would stop coming up.
    const auto nested = QStringLiteral("client\n<connection>\nremote only.example.org 1194 udp\nauth-user-pass\n</connection>\n");
    auto setting = settingFrom({{u"profile-flags"_s, u"1"_s}, {u"username"_s, u"alice"_s}, {u"password-flags"_s, u"1"_s}},
                               {{u"profile"_s, encoded(nested)}, {u"password"_s, u"stored"_s}});
    OpenVpn3SettingWidget widget(setting);

    const QVariantMap map = widget.setting();
    QCOMPARE(dataOf(map).value(u"username"_s), u"alice"_s);
    QCOMPARE(secretsOf(map).value(u"password"_s), u"stored"_s);
    QCOMPARE(dataOf(map).value(u"password-flags"_s), u"1"_s);
    QCOMPARE(dataOf(map).value(u"challenge-response-flags"_s), QString::number(NetworkManager::Setting::NotSaved));
    QCOMPARE(storedProfileOf(map), nested);

    // And without a username it is not saved: the service would refuse it.
    OpenVpn3SettingWidget nameless(settingFrom({{u"profile-flags"_s, u"1"_s}}, {{u"profile"_s, encoded(nested)}}));
    QVERIFY(!nameless.isValid());
}

void Openvpn3WidgetTest::theAuthCheckboxSaysWhenTheDirectiveIsNotItsToChange()
{
    const auto nested = QStringLiteral("client\n<connection>\nremote only.example.org 1194\nauth-user-pass\n</connection>\n");
    OpenVpn3SettingWidget widget(legacySetting(nested));

    auto *check = widget.findChild<QCheckBox *>(u"openvpn3_authuserpass"_s);
    QVERIFY(check);
    // It is checked, because the profile does ask; it is not clickable,
    // because the directive it adds and removes is not the one in there.
    QVERIFY(check->isChecked());
    QVERIFY(!check->isEnabled());
    QVERIFY(!check->toolTip().isEmpty());
    QVERIFY(widget.findChild<QLineEdit *>(u"openvpn3_username"_s)->isEnabled());
}

void Openvpn3WidgetTest::aNestedInlineCertificateStaysOpaque()
{
    // A certificate inside a <connection> block is payload, and a line of it
    // that happens to read like a directive is still payload.
    const auto nested = QStringLiteral(
        "client\n<connection>\nremote only.example.org 1194\n<ca>\nauth-user-pass\nremote not.a.directive\n</ca>\n</connection>\n");
    OpenVpn3SettingWidget widget(legacySetting(nested));

    QVERIFY(!widget.findChild<QCheckBox *>(u"openvpn3_authuserpass"_s)->isChecked());
    QCOMPARE(remotesTable(&widget)->rowCount(), 0);
    QCOMPARE(storedProfileOf(widget.setting()), nested);
    // And no username or password is stored for a profile that asks for none.
    QVERIFY(!dataOf(widget.setting()).contains(u"password-flags"_s));
}

// -- secrets --------------------------------------------------------------------

void Openvpn3WidgetTest::aStaleOneTimeCodeIsNeverSavedAgain()
{
    // A one-time code is good once. Flagging it NotSaved is not enough: the
    // whole secrets map is what gets written, so an old code left in it would
    // be handed back to the server on the next connect.
    auto setting = settingFrom({{u"profile"_s, encoded(kProfile)}, {u"username"_s, u"alice"_s}},
                               {{u"challenge-response"_s, u"123456"_s}, {u"password"_s, u"pw"_s}});
    OpenVpn3SettingWidget widget(setting);

    const QVariantMap map = widget.setting();
    QVERIFY(!secretsOf(map).contains(u"challenge-response"_s));
    QCOMPARE(dataOf(map).value(u"challenge-response-flags"_s), QString::number(NetworkManager::Setting::NotSaved));
    // The password is a different thing and stays.
    QCOMPARE(secretsOf(map).value(u"password"_s), u"pw"_s);
}

void Openvpn3WidgetTest::aWalletThatCannotStoreAnythingBlocksTheSave()
{
    Openvpn3Storage::setSecretServiceAvailability(false);
    OpenVpn3SettingWidget widget(secretSetting(u"1"_s));

    // The agent keeps agent-owned secrets in a secret service and keeps
    // nothing at all when there is none, so this profile would be lost.
    QVERIFY(!widget.isValid());
    QVERIFY(widget.blockingProblem().contains(u"wallet"_s));
    // Asking for it anyway gets back what the connection already had, not a
    // connection with its profile taken out of it.
    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
    QCOMPARE(dataOf(widget.setting()).value(u"profile-flags"_s), u"1"_s);
    // And it is not quietly moved to storage every program can read instead.
    QCOMPARE(storageCombo(&widget)->currentData().toInt(), static_cast<int>(NetworkManager::Setting::AgentOwned));
}

void Openvpn3WidgetTest::anExplicitSystemChoiceWorksWithoutAWallet()
{
    Openvpn3Storage::setSecretServiceAvailability(false);
    OpenVpn3SettingWidget widget(secretSetting(u"1"_s));
    QVERIFY(!widget.isValid());

    QComboBox *combo = storageCombo(&widget);
    combo->setCurrentIndex(combo->findData(static_cast<int>(NetworkManager::Setting::None)));

    // The way out is the user saying so, which is a real decision about who
    // can read the private keys.
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    QCOMPARE(dataOf(widget.setting()).value(u"profile-flags"_s), u"0"_s);
    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
}

void Openvpn3WidgetTest::aWalletLessPasswordIsNeverQuietlyStoredForEveryone()
{
    Openvpn3Storage::setSecretServiceAvailability(false);
    OpenVpn3SettingWidget widget(secretSetting(u"0"_s));
    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));

    auto *password = widget.findChild<PasswordField *>(u"openvpn3_password"_s);
    password->setPasswordOption(PasswordField::StoreForUser);
    password->setText(u"typed"_s);

    // The profile is system-owned and fine; the password is not, and saying
    // so beats writing it where the user did not ask for it, or dropping it.
    QVERIFY(!widget.isValid());
    QVERIFY(widget.blockingProblem().contains(u"wallet"_s));
}

void Openvpn3WidgetTest::publicProfileStorageIsNotOfferedForASecretProfile()
{
    // Keeping the keys in the connection's own settings is what older
    // versions did; offering it in the same list as the wallet turns a
    // one-click mistake into a leak of every private key in the profile.
    OpenVpn3SettingWidget widget(secretSetting());

    QCOMPARE(storageCombo(&widget)->findData(-1), -1);
    QCOMPARE(storageCombo(&widget)->count(), 2);
}

void Openvpn3WidgetTest::anExistingPublicProfileKeepsItsChoice()
{
    // An existing legacy connection still has the choice, so that opening it
    // and saving it does not relocate its profile without being asked.
    OpenVpn3SettingWidget widget(legacySetting());

    QVERIFY(storageCombo(&widget)->findData(-1) >= 0);
    QCOMPARE(storageCombo(&widget)->currentData().toInt(), -1);
}

// -- certificates and keys -------------------------------------------------------

void Openvpn3WidgetTest::embeddingAFileReplacesTheReferenceToIt()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\nremote a.example.org\nca /somewhere/ca.crt\n"_s));

    QString error;
    QVERIFY2(widget.embedFile(u"ca"_s, dataPath(u"pki/ca.crt"_s), &error), qPrintable(error));

    const QString stored = storedProfileOf(widget.setting());
    QVERIFY(!stored.contains(u"/somewhere/ca.crt"_s));
    QVERIFY(stored.contains(u"<ca>\n-----BEGIN CERTIFICATE-----"_s));
    QVERIFY(widget.hasProfileEdits());
}

void Openvpn3WidgetTest::embeddingATlsAuthKeyKeepsItsKeyDirection_data()
{
    QTest::addColumn<QString>("reference");
    QTest::addColumn<QString>("direction");

    QTest::newRow("direction 1") << u"tls-auth /somewhere/ta.key 1"_s << u"1"_s;
    QTest::newRow("direction 0") << u"tls-auth /somewhere/ta.key 0"_s << u"0"_s;
    QTest::newRow("no direction") << u"tls-auth /somewhere/ta.key"_s << QString();
}

/**
 * The direction of a tls-auth key is an argument of the reference, and an
 * inline block has nowhere to put an argument.
 *
 * OpenVPN carries it in a key-direction directive instead, which is what the
 * backend's own importer writes when it inlines one. Embedding a file here has
 * to do the same, or the key is kept and the direction is thrown away, and the
 * connection stops authenticating against a server that expects one.
 */
void Openvpn3WidgetTest::embeddingATlsAuthKeyKeepsItsKeyDirection()
{
    QFETCH(QString, reference);
    QFETCH(QString, direction);

    OpenVpn3SettingWidget widget(legacySetting(u"client\nremote a.example.org\n"_s + reference + u"\n"_s));

    QString error;
    QVERIFY2(widget.embedFile(u"tls-auth"_s, dataPath(u"pki/ta.key"_s), &error), qPrintable(error));

    const Openvpn3Profile stored = Openvpn3Profile::fromText(storedProfileOf(widget.setting()));
    QVERIFY(!stored.toText().contains(u"/somewhere/ta.key"_s));
    QVERIFY(stored.blockBody(u"tls-auth"_s).contains(u"OpenVPN Static key"_s));
    QCOMPARE(stored.value(u"key-direction"_s), direction);
    QCOMPARE(stored.contains(u"key-direction"_s), !direction.isNull());
}

void Openvpn3WidgetTest::aBinaryFileIsRefusedRatherThanMangled()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\nremote a.example.org\n"_s));

    QTemporaryDir dir;
    const QString path = dir.filePath(u"client.der"_s);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    // A DER certificate: bytes that are not UTF-8 and carry no NUL either.
    file.write(QByteArray::fromHex("30820122300d06092a864886f70d010101050003820"));
    file.write(QByteArray("\xff\xfe\xc3\x28", 4));
    file.close();

    QString error;
    QVERIFY(!widget.embedFile(u"ca"_s, path, &error));
    QVERIFY(!error.isEmpty());
    // Nothing was embedded, and in particular no block full of replacement
    // characters that is no longer the certificate it came from.
    QCOMPARE(storedProfileOf(widget.setting()), u"client\nremote a.example.org\n"_s);
    QVERIFY(!widget.hasProfileEdits());
}

void Openvpn3WidgetTest::aPkcs12BundleIsEmbeddedAsBase64()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\nremote a.example.org\n"_s));

    QTemporaryDir dir;
    const QString path = dir.filePath(u"client.p12"_s);
    const QByteArray bundle = QByteArray::fromHex("308202350201033082") + QByteArray("\x00\xff\xfe", 3);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bundle);
    file.close();

    QString error;
    QVERIFY2(widget.embedFile(u"pkcs12"_s, path, &error), qPrintable(error));

    const Openvpn3Profile profile = Openvpn3Profile::fromText(storedProfileOf(widget.setting()));
    const QString body = profile.blockBody(u"pkcs12"_s);
    QVERIFY(!body.isEmpty());
    QCOMPARE(QByteArray::fromBase64(body.trimmed().toLatin1()), bundle);
}

void Openvpn3WidgetTest::clearingRemovesEveryTraceOfTheDirective()
{
    OpenVpn3SettingWidget widget(legacySetting(u"client\nremote a.example.org\nca /a/ca.crt\n<ca>\nPEM\n</ca>\n"_s));

    widget.clearMaterial(u"ca"_s);

    QCOMPARE(storedProfileOf(widget.setting()), u"client\nremote a.example.org\n"_s);
}

// -- normalisation ---------------------------------------------------------------

void Openvpn3WidgetTest::aTypedFileReferenceIsEmbeddedOnSave()
{
    OPENVPN3_REQUIRE_BACKEND();
    // Typed on the Profile Source page: openvpn3 is handed a profile and
    // nothing else, so a file it points at has to be in it by the time it is
    // saved, however it got there.
    OpenVpn3SettingWidget widget(legacySetting());
    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(u"client\nremote a.example.org 1194 udp\nca "_s + dataPath(u"pki/ca.crt"_s) + u"\n"_s);

    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    const QString stored = storedProfileOf(widget.setting());
    QVERIFY(!stored.contains(u"pki/ca.crt"_s));
    QVERIFY(stored.contains(u"<ca>\n-----BEGIN CERTIFICATE-----"_s));
}

void Openvpn3WidgetTest::typedInlineCredentialsBecomeConnectionCredentials()
{
    OPENVPN3_REQUIRE_BACKEND();
    OpenVpn3SettingWidget widget(legacySetting());
    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(
        u"client\nremote a.example.org 1194 udp\n<auth-user-pass>\nbob\nhunter2\n</auth-user-pass>\n"_s);

    QVERIFY2(widget.isValid(), qPrintable(widget.blockingProblem()));
    const QVariantMap map = widget.setting();

    // The credentials are the connection's now, and are not left in the
    // profile where every reader of it would get them too.
    QCOMPARE(dataOf(map).value(u"username"_s), u"bob"_s);
    QCOMPARE(secretsOf(map).value(u"password"_s), u"hunter2"_s);
    const QString stored = storedProfileOf(map);
    QVERIFY(!stored.contains(u"hunter2"_s));
    QVERIFY(Openvpn3Profile::fromText(stored).containsOption(u"auth-user-pass"_s));
    // And nothing of it is anywhere a reader of the connection's data could
    // find it.
    const NMStringMap data = dataOf(map);
    for (const QString &value : data.values()) {
        QVERIFY(!value.contains(u"hunter2"_s));
    }
}

void Openvpn3WidgetTest::aProfileNamingAFileThatIsNotThereIsNotSaved()
{
    OPENVPN3_REQUIRE_BACKEND();
    OpenVpn3SettingWidget widget(legacySetting());
    tabs(&widget)->setCurrentIndex(2);
    sourceEdit(&widget)->setPlainText(u"client\nremote a.example.org 1194 udp\nca /nonexistent/nowhere.crt\n"_s);

    // Saving it would store a profile openvpn3 cannot use, with the old one
    // gone. Refusing, and saying why, is the only thing left.
    QVERIFY(!widget.isValid());
    QVERIFY(widget.blockingProblem().contains(u"nowhere.crt"_s));
    // Not half of it either: the connection comes back as it was.
    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
}

void Openvpn3WidgetTest::anUntouchedProfileIsNeverRewritten()
{
    OPENVPN3_REQUIRE_BACKEND();
    // Normalising is for profiles that need it. One that is already
    // self-contained comes out byte for byte as it went in, duplicate
    // directives, odd quoting and all.
    OpenVpn3SettingWidget widget(legacySetting());

    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
}

// -- reimport --------------------------------------------------------------------

void Openvpn3WidgetTest::aReimportKeepsAnUnsavedStorageChoice()
{
    OPENVPN3_REQUIRE_BACKEND();
    // The choice was made and not saved yet; an import replaces the
    // configuration, and the policy is not part of the configuration.
    OpenVpn3SettingWidget widget(secretSetting(u"1"_s));
    QComboBox *combo = storageCombo(&widget);
    combo->setCurrentIndex(combo->findData(static_cast<int>(NetworkManager::Setting::None)));
    widget.findChild<PasswordField *>(u"openvpn3_password"_s)->setPasswordOption(PasswordField::StoreForAllUsers);

    QString error;
    QVERIFY2(widget.importProfile(dataPath(u"office.ovpn"_s), &error), qPrintable(error));

    // Both choices stand: where the profile goes and where the password goes
    // are the user's, and the file has nothing to say about either.
    QCOMPARE(combo->currentData().toInt(), static_cast<int>(NetworkManager::Setting::None));
    const QVariantMap map = widget.setting();
    QCOMPARE(dataOf(map).value(u"profile-flags"_s), u"0"_s);
    QCOMPARE(dataOf(map).value(u"password-flags"_s), u"0"_s);
    QCOMPARE(secretsOf(map).value(u"password"_s), u"correct-horse-battery-staple"_s);
}

void Openvpn3WidgetTest::aReimportIsWorthConfirmingWhenCredentialsAreStored()
{
    // Nothing was edited, so there is no unsaved work to lose -- but the
    // stored password was for the old server, and the new profile may be for
    // another one entirely.
    auto setting = settingFrom({{u"profile"_s, encoded(kProfile)}, {u"username"_s, u"alice"_s}}, {{u"password"_s, u"stored"_s}});
    OpenVpn3SettingWidget widget(setting);

    QVERIFY(!widget.hasUnsavedEdits());
    QVERIFY(widget.importWouldReplaceStoredCredentials());

    OpenVpn3SettingWidget withoutCredentials(legacySetting(u"client\nremote a.example.org\n"_s));
    QVERIFY(!withoutCredentials.importWouldReplaceStoredCredentials());
}

QTEST_MAIN(Openvpn3WidgetTest)

#include "openvpn3widgettest.moc"
