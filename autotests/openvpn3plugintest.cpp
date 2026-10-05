/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <memory>

#include <QTest>

#include <KPluginFactory>
#include <KPluginMetaData>

#include <NetworkManagerQt/VpnSetting>

#include "nm-openvpn3-service.h"
#include "openvpn3widget.h"
#include "settingwidget.h"
#include "vpnuiplugin.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
NetworkManager::VpnSetting::Ptr someSetting()
{
    NetworkManager::VpnSetting source;
    source.setServiceType(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    source.setData({{u"profile"_s, QString::fromLatin1(QByteArray("client\nremote vpn.example.org 1194\n").toBase64())}});

    auto setting = NetworkManager::VpnSetting::Ptr(new NetworkManager::VpnSetting);
    setting->fromMap(source.toMap());
    return setting;
}

/**
 * Every installed VPN plugin that claims @p extension, found the way a stock
 * plasma-nm's top-level import finds one.
 *
 * kcm.cpp walks KPluginMetaData::findPlugins("plasma/network/vpn"),
 * instantiates each plugin and asks whether supportedFileExtensions()
 * contains "*.<suffix>". This is that loop, so what it reports is what that
 * import would do.
 */
QStringList servicesClaiming(const QString &extension)
{
    QStringList services;
    const QList<KPluginMetaData> plugins = KPluginMetaData::findPlugins(u"plasma/network/vpn"_s);
    for (const KPluginMetaData &metaData : plugins) {
        const auto result = KPluginFactory::instantiatePlugin<VpnUiPlugin>(metaData);
        if (!result) {
            continue;
        }
        std::unique_ptr<VpnUiPlugin> plugin(result.plugin);
        if (plugin->supportedFileExtensions().contains(u"*."_s + extension)) {
            services.append(metaData.value(u"X-NetworkManager-Services"_s));
        }
    }
    return services;
}
}

/**
 * The plugin as the rest of Plasma sees it: found by service type, loaded out
 * of the shared object, and able to produce both of its widgets.
 *
 * This is the step the unit tests cannot cover, because they link the code
 * directly; here the metadata, the factory macro and the install location all
 * have to be right, and the plugin is loaded into a process alongside the
 * distribution's own plasma-nm library.
 */
class Openvpn3PluginTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theMetadataNamesTheService();
    void thePluginLoadsForItsServiceType();
    void thePluginProducesItsWidgets();
    void theTopLevelImportIsLeftToOpenVpn2();
    void exportIsRefusedRatherThanLeakingTheKeys();
    void emptySecretLoadsFailClosed_data();
    void emptySecretLoadsFailClosed();
};

void Openvpn3PluginTest::theMetadataNamesTheService()
{
    const QList<KPluginMetaData> plugins = KPluginMetaData::findPlugins(u"plasma/network/vpn"_s, [](const KPluginMetaData &data) {
        return data.value(u"X-NetworkManager-Services"_s) == QLatin1String(NM_DBUS_SERVICE_OPENVPN3);
    });

    QCOMPARE(plugins.size(), 1);
    QCOMPARE(plugins.constFirst().name(), u"OpenVPN 3"_s);
}

void Openvpn3PluginTest::thePluginLoadsForItsServiceType()
{
    const auto result = VpnUiPlugin::loadPluginForType(nullptr, QLatin1String(NM_DBUS_SERVICE_OPENVPN3));

    QVERIFY2(result, qPrintable(result.errorString));
    delete result.plugin;
}

void Openvpn3PluginTest::thePluginProducesItsWidgets()
{
    const auto result = VpnUiPlugin::loadPluginForType(nullptr, QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    QVERIFY2(result, qPrintable(result.errorString));
    std::unique_ptr<VpnUiPlugin> plugin(result.plugin);

    const auto setting = someSetting();
    std::unique_ptr<SettingWidget> editor(plugin->widget(setting, nullptr));
    QVERIFY(editor);
    QCOMPARE(editor->type(), u"vpn"_s);
    QVERIFY(editor->isValid());

    std::unique_ptr<SettingWidget> prompt(plugin->askUser(setting, {u"challenge-response"_s}, nullptr));
    QVERIFY(prompt);
}

/**
 * Installing this module must not change what happens to somebody else's
 * .ovpn file.
 *
 * A stock plasma-nm's top-level import takes the first plugin that claims the
 * extension, in whatever order the plugins were found; both OpenVPN plugins
 * read .ovpn, and the host has no way to ask which was meant. So this plugin
 * claims nothing, and that is asserted against the real installed plugins
 * rather than against this one in isolation: the distribution's OpenVPN 2
 * plugin has to still be the one that answers.
 */
void Openvpn3PluginTest::theTopLevelImportIsLeftToOpenVpn2()
{
    const auto result = VpnUiPlugin::loadPluginForType(nullptr, QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    QVERIFY(result);
    std::unique_ptr<VpnUiPlugin> plugin(result.plugin);
    QVERIFY(plugin->supportedFileExtensions().isEmpty());

    for (const QString &extension : {u"ovpn"_s, u"conf"_s}) {
        const QStringList services = servicesClaiming(extension);
        QVERIFY2(!services.contains(QLatin1String(NM_DBUS_SERVICE_OPENVPN3)), qPrintable(extension + u": "_s + services.join(u", "_s)));
    }
    // And the plugin that did claim .ovpn before this module was installed
    // still does, so the import it answers is unchanged.
    QVERIFY2(servicesClaiming(u"ovpn"_s).contains(u"org.freedesktop.NetworkManager.openvpn"_s),
             "the distribution's OpenVPN 2 plugin is not installed, so this assertion proves nothing");
}

void Openvpn3PluginTest::exportIsRefusedRatherThanLeakingTheKeys()
{
    const auto result = VpnUiPlugin::loadPluginForType(nullptr, QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    QVERIFY(result);
    std::unique_ptr<VpnUiPlugin> plugin(result.plugin);

    auto connection = NetworkManager::ConnectionSettings::Ptr(new NetworkManager::ConnectionSettings(NetworkManager::ConnectionSettings::Vpn));
    const auto exported = plugin->exportConnectionSettings(connection, u"/nonexistent/should-not-be-written.ovpn"_s);

    QVERIFY(!exported);
    QVERIFY(!exported.errorMessage().isEmpty());
}

void Openvpn3PluginTest::emptySecretLoadsFailClosed_data()
{
    QTest::addColumn<bool>("emptyValue");
    QTest::addColumn<bool>("alreadyLoaded");
    QTest::newRow("missing-locked") << false << false;
    QTest::newRow("empty-locked") << true << false;
    QTest::newRow("missing-loaded") << false << true;
    QTest::newRow("empty-loaded") << true << true;
}

void Openvpn3PluginTest::emptySecretLoadsFailClosed()
{
    QFETCH(bool, emptyValue);
    QFETCH(bool, alreadyLoaded);
    const auto result = VpnUiPlugin::loadPluginForType(nullptr, QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    QVERIFY(result);
    std::unique_ptr<VpnUiPlugin> plugin(result.plugin);
    auto vpn = someSetting();
    const QString original = vpn->data().value(u"profile"_s);
    // A stale public value must not become a fallback.
    vpn->setData({{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"0"_s}, {u"profile"_s, original}});
    if (alreadyLoaded) {
        vpn->setSecrets({{u"profile"_s, original}});
    }
    std::unique_ptr<SettingWidget> widget(plugin->widget(vpn, nullptr));
    const auto before = widget->setting();
    auto reply = someSetting();
    reply->setSecrets(emptyValue ? NMStringMap{{u"profile"_s, QString()}} : NMStringMap{});
    widget->loadSecrets(reply);
    const auto after = widget->setting();
    if (alreadyLoaded) {
        QVERIFY(widget->isValid());
        QCOMPARE(qdbus_cast<NMStringMap>(after.value(u"secrets"_s)).value(u"profile"_s), original);
    } else {
        QVERIFY(!widget->isValid());
        QCOMPARE(after, before);
        QVERIFY(!qdbus_cast<NMStringMap>(after.value(u"secrets"_s)).contains(u"profile"_s));
    }
}

QTEST_MAIN(Openvpn3PluginTest)

#include "openvpn3plugintest.moc"
