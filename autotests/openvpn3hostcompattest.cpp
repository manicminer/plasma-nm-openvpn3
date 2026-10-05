/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <QCheckBox>
#include <QTest>

#include <NetworkManagerQt/ConnectionSettings>
#include <NetworkManagerQt/VpnSetting>

#include "connectioneditorbase.h"
#include "nm-openvpn3-service.h"
#include "openvpn3storage.h"
#include "openvpn3widget.h"
#include "settingwidget.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
/**
 * The host's own connection editor, driven headlessly.
 *
 * ConnectionEditorBase is where plasma-nm decides which pages a connection
 * gets and what the saved connection is made of, and it is abstract only in
 * where the pages go and what the name is. Subclassing it is therefore the
 * way to ask the installed plasma-nm what it would do, rather than reading its
 * source and believing the answer.
 */
class HostEditor : public ConnectionEditorBase
{
public:
    explicit HostEditor(const NetworkManager::ConnectionSettings::Ptr &connection)
        : ConnectionEditorBase(connection)
    {
        initialize();
    }

    QStringList pages;

protected:
    void addWidget(QWidget *widget, const QString &text) override
    {
        widget->setParent(this);
        pages.append(text);
    }

    QString connectionName() const override
    {
        return u"edited name"_s;
    }
};

NetworkManager::ConnectionSettings::Ptr vpnConnection(const QString &serviceType, const NMStringMap &data = {})
{
    auto settings = NetworkManager::ConnectionSettings::Ptr(new NetworkManager::ConnectionSettings(NetworkManager::ConnectionSettings::Vpn));
    settings->setId(u"original name"_s);
    settings->setUuid(u"24f1ab6c-72a1-4a01-a898-de153b0f7564"_s);

    NetworkManager::VpnSetting vpn;
    vpn.setServiceType(serviceType);
    vpn.setData(data);
    auto map = settings->toMap();
    map[u"vpn"_s] = vpn.toMap();
    settings->fromMap(map);
    return settings;
}
}

/**
 * What a stock, unmodified plasma-nm does with an OpenVPN 3 connection.
 *
 * Not tests of this module: tests of the host it is installed into. Each one
 * pins a limitation that is documented in docs/limitations.md, so that a
 * plasma-nm which fixes one makes an assertion here fail and the
 * documentation gets corrected instead of quietly going stale. They are the
 * reason it is possible to say what this module cannot do without guessing.
 *
 * Everything here is measured against the plasma-nm the module is pinned to;
 * see provenance/plasma-nm-6.7.5.pin.
 */
class Openvpn3HostCompatTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theHostGivesAnOpenVpn3ConnectionNoIpv6Page();
    void theHostDropsConnectionPropertiesItDoesNotShow();
    void whatThisModuleWritesSatisfiesTheHostsSecretRequestPredicate_data();
    void whatThisModuleWritesSatisfiesTheHostsSecretRequestPredicate();
};

/**
 * Stock plasma-nm offers a VPN an IPv6 page only for OpenVPN 2.
 *
 * The page is not cosmetic: without it the connection's IPv6 settings are
 * neither shown nor editable, and the editor writes back whatever default the
 * connection type carries. Which VPN services get one is decided inside
 * ConnectionEditorBase by service name, so no plugin can add itself to that
 * list: a setting widget's map is inserted under its own type, and the page
 * list is private.
 */
void Openvpn3HostCompatTest::theHostGivesAnOpenVpn3ConnectionNoIpv6Page()
{
    HostEditor openvpn3(vpnConnection(QLatin1String(NM_DBUS_SERVICE_OPENVPN3)));
    // The enumeration works, and this module's page is among them -- otherwise
    // an empty page list would satisfy the assertion below for the wrong
    // reason.
    QVERIFY2(openvpn3.pages.contains(u"IPv4"_s), qPrintable(openvpn3.pages.join(u", "_s)));
    QVERIFY2(!openvpn3.pages.contains(u"IPv6"_s), qPrintable(openvpn3.pages.join(u", "_s)));

    // And it is the service type that decides, not VPNs in general: the host
    // does give OpenVPN 2 one. If this ever starts passing for OpenVPN 3,
    // docs/limitations.md is out of date and should be corrected.
    HostEditor openvpn2(vpnConnection(u"org.freedesktop.NetworkManager.openvpn"_s));
    QVERIFY2(openvpn2.pages.contains(u"IPv6"_s), qPrintable(openvpn2.pages.join(u", "_s)));
}

/**
 * Stock plasma-nm drops the connection properties its General page does not
 * show, on every save, for every connection type.
 *
 * ConnectionWidget::setting() builds a fresh ConnectionSettings and fills in
 * what its own controls hold, so anything else a connection carried --
 * interface-name, mdns, autoconnect-retries and the rest -- is gone from what
 * the editor saves. This has nothing to do with OpenVPN 3 and is not made
 * worse by installing this module; what this asserts is that it is the host
 * doing it, and that this module's own page contributes nothing but the VPN
 * setting, so the loss cannot be laid at its door or fixed from inside it.
 */
void Openvpn3HostCompatTest::theHostDropsConnectionPropertiesItDoesNotShow()
{
    auto settings = vpnConnection(QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    settings->setInterfaceName(u"reviewtun"_s);
    auto map = settings->toMap();
    map[u"connection"_s][u"autoconnect-retries"_s] = 7;
    map[u"connection"_s][u"gateway-ping-timeout"_s] = uint(1234);
    map[u"connection"_s][u"mdns"_s] = 2;
    settings->fromMap(map);
    const QVariantMap original = settings->toMap().value(u"connection"_s);

    HostEditor editor(settings);
    const QVariantMap saved = editor.setting().value(u"connection"_s);

    // What the host does keep, and what it is for: the connection is still the
    // same connection, and the name the editor holds wins.
    QCOMPARE(saved.value(u"uuid"_s), original.value(u"uuid"_s));
    QCOMPARE(saved.value(u"id"_s).toString(), u"edited name"_s);

    // What it loses. Asserted rather than described, so that a plasma-nm which
    // starts preserving these fails here and docs/limitations.md gets fixed.
    for (const QString &key : {u"interface-name"_s, u"autoconnect-retries"_s, u"gateway-ping-timeout"_s, u"mdns"_s}) {
        QVERIFY2(original.contains(key), qPrintable(key));
        QVERIFY2(saved.value(key) != original.value(key), qPrintable(key + u" is no longer dropped by the host"_s));
    }

    // And this module's page is not where it goes: it offers the VPN setting
    // and nothing else, so there is no connection map of its own to be blamed
    // or to fix it with. The page has to be found for that to mean anything --
    // a host that loaded no plugin at all would satisfy a loop that never ran.
    SettingWidget *vpnPage = nullptr;
    for (SettingWidget *widget : editor.findChildren<SettingWidget *>()) {
        if (widget->type() == u"vpn"_s) {
            vpnPage = widget;
        }
    }
    QVERIFY2(vpnPage, "the host did not load this module's page, so nothing here was measured against it");
    QCOMPARE(QString::fromLatin1(vpnPage->metaObject()->className()), u"OpenVpn3SettingWidget"_s);
    // Its map is inserted under its own type, "vpn", and holds nothing but a
    // VPN setting's own keys. There is no way for it to contribute a
    // connection map, which is why this is the host's to fix and not its.
    QStringList offered = vpnPage->setting().keys();
    offered.sort();
    for (const QString &key : std::as_const(offered)) {
        // NetworkManager::VpnSetting's own keys, and no others.
        static const QStringList vpnKeys{u"service-type"_s, u"user-name"_s, u"data"_s, u"secrets"_s, u"persistent"_s, u"timeout"_s};
        QVERIFY2(vpnKeys.contains(key), qPrintable(key));
    }
}

void Openvpn3HostCompatTest::whatThisModuleWritesSatisfiesTheHostsSecretRequestPredicate_data()
{
    QTest::addColumn<int>("profileFlags");
    QTest::newRow("kept in the wallet") << int(NetworkManager::Setting::AgentOwned);
    QTest::newRow("kept for all users") << int(NetworkManager::Setting::None);
}

/**
 * What this module writes satisfies the condition under which the host asks
 * NetworkManager for a connection's secrets.
 *
 * This is what makes the module work on a stock host at all: the profile is a
 * secret, so a connection that recorded nothing about the secrets it keeps
 * would open in the editor with no profile and nothing to say why.
 *
 * Measured against Openvpn3Storage::hostRequestsSecrets(), which is this
 * project's reimplementation of the host's predicate and is pinned to
 * plasma-nm 6.7.5 like the headers -- not against a live host. A real
 * GetSecrets round trip needs a NetworkManager to make it to, and these are
 * component tests with no bus. So this asserts the contract, not the
 * conversation: if a future plasma-nm changed the predicate, this would keep
 * passing and the reimplementation, not this assertion, is what would have to
 * be revisited. The two unlike it above do measure the installed host.
 */
void Openvpn3HostCompatTest::whatThisModuleWritesSatisfiesTheHostsSecretRequestPredicate()
{
    QFETCH(int, profileFlags);

    NMStringMap data;
    NMStringMap secrets;
    QVERIFY(Openvpn3Storage::writeSecretProfile(data,
                                                secrets,
                                                u"client\nremote vpn.example.org 1194\n"_s,
                                                static_cast<NetworkManager::Setting::SecretFlags>(profileFlags)));

    QVERIFY(Openvpn3Storage::recordsProfileFlags(data));
    QVERIFY(Openvpn3Storage::hostRequestsSecrets(data));

    // The whole profile is in the secrets and no stale public copy is left in
    // the data for anything to read instead.
    QVERIFY(secrets.contains(u"profile"_s));
    QVERIFY(!data.contains(u"profile"_s));
}

QTEST_MAIN(Openvpn3HostCompatTest)

#include "openvpn3hostcompattest.moc"
