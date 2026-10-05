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

/** A connection written before the formatting was dropped, or a file that
 * still has it: comments and blank lines the editor has to cope with without
 * showing them. */
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

/** kCommented once it has been through the editor: the comments and the blank
 * lines between directives are gone, the blank line inside the certificate is
 * payload and stays. */
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

/** A VPN setting whose profile is one of the connection's secrets. */
NetworkManager::VpnSetting::Ptr secretSetting(const QString &flags = u"1"_s, bool withSecret = true, const QString &profile = kProfile)
{
    const NMStringMap data{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, flags}, {u"username"_s, u"alice"_s}};
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

/** True when the openvpn3 backend's libnm plugin is available to libnm. */
bool backendAvailable()
{
    static const bool available = Openvpn3Importer::fromFile(dataPath(u"office.ovpn"_s)).isValid();
    return available;
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
        QVERIFY(backendAvailable()); // These regressions require the real importer.
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
        if (!backendAvailable()) {
            QSKIP("the openvpn3 backend's libnm plugin is not installed");
        }
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
    void aCommentTypedIntoTheSourceIsDropped();
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
    void commentsInAStoredProfileAreNeitherShownNorSavedBack();
    void blankLinesInAStoredProfileAreNeitherShownNorSavedBack();
    void aCommentTypedIntoAScopeBodyIsNotSaved();
    void aBlankLineTypedIntoTheSourceIsNotSaved();
    void aBlankLineTypedIntoAScopeBodyIsNotSaved();
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
    QCOMPARE(dataOf(map).value(u"profile-storage"_s), u"secret"_s);
    QCOMPARE(dataOf(map).value(u"profile-flags"_s), u"1"_s);
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

void Openvpn3WidgetTest::aCommentTypedIntoTheSourceIsDropped()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QPlainTextEdit *source = sourceEdit(&widget);
    QVERIFY(source);

    tabs(&widget)->setCurrentIndex(2);
    source->setPlainText(u"# a comment of mine\nclient\nremote typed.example.org 1234 tcp # here\n"_s);

    // The profile is not a file anybody will open again, so a comment typed
    // into it has nowhere to live; the rest of the text is taken as typed.
    QCOMPARE(storedProfileOf(widget.setting()), u"client\nremote typed.example.org 1234 tcp\n"_s);
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
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
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
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
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

void Openvpn3WidgetTest::commentsInAStoredProfileAreNeitherShownNorSavedBack()
{
    // A connection imported by an older build still carries its comments and
    // its blank lines.  Opening it is not supposed to show rows for them, and
    // saving it is not supposed to write them back.
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

    // The quoted hash is a value, the one in the certificate is payload and so
    // is the blank line in there; only the formatting between directives went.
    QCOMPARE(widget.profileText(), kCommentedKept);
    QCOMPARE(storedProfileOf(widget.setting()), kCommentedKept);

    // And the editor still reads the profile the same way afterwards.
    QTableWidget *remotes = remotesTable(&widget);
    QCOMPARE(remotes->rowCount(), 1);
    QCOMPARE(remotes->item(0, 0)->text(), u"vpn1.example.org"_s);
}

void Openvpn3WidgetTest::aCommentTypedIntoAScopeBodyIsNotSaved()
{
    // The source page drops a comment typed into it; the directive table has
    // to drop one typed into a <connection> body too, or the same comment
    // would be saved on one page and refused on the other.
    OpenVpn3SettingWidget widget(legacySetting(u"client\n<connection>\nremote a.example.org 1194 udp\n</connection>\n"_s));
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);

    const int row = rowOfDirective(table, u"connection"_s);
    QVERIFY(row >= 0);
    table->selectRow(row);

    auto *body = widget.findChild<QPlainTextEdit *>(u"openvpn3_directives_body"_s);
    QVERIFY(body);
    body->setPlainText(u"# mine\nremote b.example.org 443 tcp # here\n"_s);

    const auto expected = u"client\n<connection>\nremote b.example.org 443 tcp\n</connection>\n"_s;
    QCOMPARE(widget.profileText(), expected);
    QCOMPARE(storedProfileOf(widget.setting()), expected);
}

void Openvpn3WidgetTest::blankLinesInAStoredProfileAreNeitherShownNorSavedBack()
{
    // The same for a connection that still has blank lines in it: no rows for
    // them, and saving does not write them back.  The ones inside the opaque
    // payload are content and both the table and the save path leave them be.
    const auto stored = u"\nclient\n\nremote vpn1.example.org 1194 udp\n   \n<ca>\n\nPEM\n\n</ca>\n\n"_s;
    const auto kept = u"client\nremote vpn1.example.org 1194 udp\n<ca>\n\nPEM\n\n</ca>\n"_s;
    OpenVpn3SettingWidget widget(legacySetting(stored));
    tabs(&widget)->setCurrentIndex(1);
    QTableWidget *table = directivesTable(&widget);

    QCOMPARE(table->rowCount(), 3); // client, remote, <ca>
    for (int row = 0; row < table->rowCount(); ++row) {
        QVERIFY(table->item(row, 0)->text() != u"Blank line"_s);
        QVERIFY(!table->item(row, 1)->text().isEmpty());
    }
    QCOMPARE(widget.profileText(), kept);
    QCOMPARE(storedProfileOf(widget.setting()), kept);

    // Visiting every page and coming back is not an edit, and saving again
    // changes nothing more: the document is a fixed point.
    for (int tab = 0; tab < tabs(&widget)->count(); ++tab) {
        tabs(&widget)->setCurrentIndex(tab);
    }
    QCOMPARE(storedProfileOf(widget.setting()), kept);
    OpenVpn3SettingWidget again(legacySetting(storedProfileOf(widget.setting())));
    QCOMPARE(storedProfileOf(again.setting()), kept);
}

void Openvpn3WidgetTest::aBlankLineTypedIntoTheSourceIsNotSaved()
{
    OpenVpn3SettingWidget widget(legacySetting());
    QPlainTextEdit *source = sourceEdit(&widget);
    QVERIFY(source);

    tabs(&widget)->setCurrentIndex(2);
    source->setPlainText(u"\nclient\n\nremote typed.example.org 1234 tcp\n  \n"_s);

    // The profile is not a file anybody will open again, so a blank line typed
    // into it has nowhere to live; the rest of the text is taken as typed.
    QCOMPARE(storedProfileOf(widget.setting()), u"client\nremote typed.example.org 1234 tcp\n"_s);

    // And the page shows what was actually kept, rather than leaving the user
    // looking at lines the connection does not have.
    tabs(&widget)->setCurrentIndex(0);
    QCOMPARE(source->toPlainText(), u"client\nremote typed.example.org 1234 tcp\n"_s);
}

void Openvpn3WidgetTest::aBlankLineTypedIntoAScopeBodyIsNotSaved()
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

    const auto expected = u"client\n<connection>\nremote b.example.org 443 tcp\n</connection>\n"_s;
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
    // openvpn3 reads <connection> as options, so a profile can ask for a
    // username and password in there and need them just as much. Looking only
    // at the top level would throw the stored credentials away on the next
    // save, and the connection would stop coming up.
    const auto nested = QStringLiteral("client\n<connection>\nremote only.example.org 1194 udp\nauth-user-pass\n</connection>\n");
    auto setting = settingFrom({{u"profile"_s, encoded(nested)}, {u"username"_s, u"alice"_s}, {u"password-flags"_s, u"1"_s}},
                               {{u"password"_s, u"stored"_s}});
    OpenVpn3SettingWidget widget(setting);

    const QVariantMap map = widget.setting();
    QCOMPARE(dataOf(map).value(u"username"_s), u"alice"_s);
    QCOMPARE(secretsOf(map).value(u"password"_s), u"stored"_s);
    QCOMPARE(dataOf(map).value(u"password-flags"_s), u"1"_s);
    QCOMPARE(dataOf(map).value(u"challenge-response-flags"_s), QString::number(NetworkManager::Setting::NotSaved));
    QCOMPARE(storedProfileOf(map), nested);
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
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
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
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
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
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
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
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    // Normalising is for profiles that need it. One that is already
    // self-contained comes out byte for byte as it went in, duplicate
    // directives, odd quoting and all.
    OpenVpn3SettingWidget widget(legacySetting());

    QCOMPARE(storedProfileOf(widget.setting()), kProfile);
}

// -- reimport --------------------------------------------------------------------

void Openvpn3WidgetTest::aReimportKeepsAnUnsavedStorageChoice()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
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
