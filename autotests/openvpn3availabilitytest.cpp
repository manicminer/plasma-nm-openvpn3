/* SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include <QApplication>
#include <QDBusConnection>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include "openvpn3storage.h"
#include "openvpn3widget.h"
#include "passwordfield.h"
#include "vpnuiplugin.h"
#include <libnm/NetworkManager.h>
#include <memory>

class Openvpn3AvailabilityTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void providerReachability_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("no-session-bus") << 0;
        QTest::newRow("bus-without-provider") << 1;
        QTest::newRow("registered-provider") << 2;
        QTest::newRow("activatable-provider") << 3;
    }
    void providerReachability()
    {
        QFETCH(int, mode);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile service(dir.filePath(QStringLiteral("org.freedesktop.secrets.service")));
        if (mode == 3) {
            QVERIFY(service.open(QIODevice::WriteOnly));
            service.write("[D-BUS Service]\nName=org.freedesktop.secrets\nExec=/bin/false\n");
            service.close();
        }
        QFile config(dir.filePath(QStringLiteral("bus.conf")));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write((QStringLiteral("<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><servicedir>")
                      + dir.path() + QStringLiteral("</servicedir><policy context=\"default\"><allow user=\"*\"/><allow own=\"*\"/><allow send_destination=\"*\"/><allow receive_sender=\"*\"/></policy></busconfig>")).toUtf8());
        config.close();
        QProcess daemon;
        QString address = QStringLiteral("unix:path=/nonexistent/openvpn3-test-bus");
        if (mode != 0) {
            daemon.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--nofork"), QStringLiteral("--print-address=1"), QStringLiteral("--config-file=") + config.fileName()});
            QVERIFY(daemon.waitForStarted());
            QVERIFY(daemon.waitForReadyRead());
            address = QString::fromUtf8(daemon.readLine()).trimmed();
        }
        {
            auto bus = QDBusConnection::connectToBus(address, QStringLiteral("provider-test-%1").arg(mode));
            if (mode == 2) {
                QVERIFY(bus.registerService(QStringLiteral("org.freedesktop.secrets")));
            }
            QProcess child;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), address);
            child.setProcessEnvironment(env);
            child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--probe"), mode >= 2 ? QStringLiteral("yes") : QStringLiteral("no")});
            QVERIFY(child.waitForFinished());
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QCOMPARE(child.exitCode(), 0);
            QDBusConnection::disconnectFromBus(QStringLiteral("provider-test-%1").arg(mode));
        }
        if (daemon.state() != QProcess::NotRunning) {
            daemon.terminate();
            QVERIFY(daemon.waitForFinished());
        }
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--probe"))) {
        const bool expected = app.arguments().last() == QStringLiteral("yes");
        if (Openvpn3Storage::secretServiceIsAvailable() != expected) {
            return 1;
        }
        OpenVpn3SettingWidget widget(NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting));
        for (const auto *field : widget.findChildren<PasswordField *>()) {
            if (field->passwordOption() != PasswordField::StoreForUser) {
                return 2;
            }
        }
        const auto loaded = VpnUiPlugin::loadPluginForType(nullptr, QStringLiteral("org.freedesktop.NetworkManager.openvpn3"));
        if (!loaded) {
            return 3;
        }
        std::unique_ptr<VpnUiPlugin> plugin(loaded.plugin);
        const auto result = plugin->importConnectionSettings(QString::fromLatin1(OPENVPN3_TEST_DATA_DIR) + QStringLiteral("/office.ovpn"));
        if (bool(result) != expected) {
            return 4;
        }
        if (result) {
            NMSettingVpn *vpn = nm_connection_get_setting_vpn(result.connection());
            const bool safe = !nm_setting_vpn_get_data_item(vpn, "profile")
                && QByteArray(nm_setting_vpn_get_data_item(vpn, "profile-flags")) == "1"
                && QByteArray(nm_setting_vpn_get_data_item(vpn, "password-flags")) == "1"
                && nm_setting_vpn_get_secret(vpn, "profile") && nm_setting_vpn_get_secret(vpn, "password");
            g_object_unref(result.connection());
            if (!safe) {
                return 5;
            }
        } else if (!result.errorMessage().contains(QStringLiteral("wallet"))) {
            return 6;
        }
        return 0;
    }
    Openvpn3AvailabilityTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "openvpn3availabilitytest.moc"
