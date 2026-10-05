/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <QTest>

#include <optional>

#include "nm-openvpn3-service.h"
#include "openvpn3storage.h"

using namespace Qt::Literals::StringLiterals;
using Flags = NetworkManager::Setting::SecretFlags;

namespace
{
const auto kProfile = QStringLiteral(
    "client\n"
    "remote vpn.example.net 1194\n"
    "auth-user-pass\n"
    "<key>\n"
    "-----BEGIN PRIVATE KEY-----\n"
    "VERYSECRET\n"
    "-----END PRIVATE KEY-----\n"
    "</key>\n");

QString encoded(const QString &profile)
{
    return QString::fromLatin1(profile.toUtf8().toBase64());
}

/** Every value a NetworkManager connection file would end up holding. */
QStringList publicValues(const NMStringMap &data)
{
    return data.values();
}
}

class Openvpn3StorageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readsALegacyProfile();
    void readsASecretProfile();
    void neverReadsAStalePublicCopyInSecretMode();
    void reportsWhyAProfileIsMissing();
    void reportsACorruptProfile();
    void writesTheLegacyLayout();
    void writesTheWalletLayout();
    void writesTheSystemLayout();
    void migratingToSecretRemovesThePublicCopy();
    void migratingBackRemovesTheSecret();
    void keyMaterialNeverReachesTheDataMap();
    void refusesToWriteFlagsThatWouldNotStoreTheProfile();
    void writersDefaultToTheWallet();
    void absentFlagsMeanSystemStorage();
    void refusesToReadFlagsThatWouldNotStoreTheProfile_data();
    void refusesToReadFlagsThatWouldNotStoreTheProfile();
    void unknownStorageMarkerFailsClosed();
    void emptyStorageMarkerIsTheLegacyLayout();
    void clearRemovesEveryTrace();
};

void Openvpn3StorageTest::readsALegacyProfile()
{
    const NMStringMap data{{u"profile"_s, encoded(kProfile)}};

    QVERIFY(!Openvpn3Storage::isSecretMode(data));
    QCOMPARE(Openvpn3Storage::availability(data, {}), Openvpn3Storage::Availability::Available);
    QCOMPARE(Openvpn3Storage::readProfile(data, {}), kProfile);
    QCOMPARE(Openvpn3Storage::profileFlags(data), std::optional<Flags>(NetworkManager::Setting::None));
}

void Openvpn3StorageTest::readsASecretProfile()
{
    const NMStringMap data{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}};
    const NMStringMap secrets{{u"profile"_s, encoded(kProfile)}};

    QVERIFY(Openvpn3Storage::isSecretMode(data));
    QCOMPARE(Openvpn3Storage::availability(data, secrets), Openvpn3Storage::Availability::Available);
    QCOMPARE(Openvpn3Storage::readProfile(data, secrets), kProfile);
    QCOMPARE(Openvpn3Storage::profileFlags(data), std::optional<Flags>(NetworkManager::Setting::AgentOwned));
}

void Openvpn3StorageTest::neverReadsAStalePublicCopyInSecretMode()
{
    const NMStringMap data{
        {u"profile-storage"_s, u"secret"_s},
        {u"profile-flags"_s, u"1"_s},
        {u"profile"_s, encoded(QStringLiteral("client\nremote stale.example.net\n"))},
    };

    QCOMPARE(Openvpn3Storage::availability(data, {}), Openvpn3Storage::Availability::Locked);
    QVERIFY(Openvpn3Storage::readProfile(data, {}).isEmpty());
}

void Openvpn3StorageTest::reportsWhyAProfileIsMissing()
{
    QCOMPARE(Openvpn3Storage::availability({}, {}), Openvpn3Storage::Availability::Absent);

    const NMStringMap locked{{u"profile-storage"_s, u"secret"_s}};
    QCOMPARE(Openvpn3Storage::availability(locked, {}), Openvpn3Storage::Availability::Locked);
}

void Openvpn3StorageTest::reportsACorruptProfile()
{
    const NMStringMap data{{u"profile"_s, u"@@ not base64 @@"_s}};
    QCOMPARE(Openvpn3Storage::availability(data, {}), Openvpn3Storage::Availability::Corrupt);
    QVERIFY(Openvpn3Storage::readProfile(data, {}).isEmpty());

    const NMStringMap secretData{{u"profile-storage"_s, u"secret"_s}};
    const NMStringMap secrets{{u"profile"_s, QString::fromLatin1(QByteArray("\xff\xfe\x00", 3).toBase64())}};
    QCOMPARE(Openvpn3Storage::availability(secretData, secrets), Openvpn3Storage::Availability::Corrupt);
}

void Openvpn3StorageTest::writesTheLegacyLayout()
{
    NMStringMap data;
    NMStringMap secrets;

    Openvpn3Storage::writeLegacyProfile(data, secrets, kProfile);

    QCOMPARE(data.value(u"profile"_s), encoded(kProfile));
    QVERIFY(!data.contains(u"profile-storage"_s));
    QVERIFY(!data.contains(u"profile-flags"_s));
    QVERIFY(!secrets.contains(u"profile"_s));
    QCOMPARE(Openvpn3Storage::readProfile(data, secrets), kProfile);
}

void Openvpn3StorageTest::writesTheWalletLayout()
{
    NMStringMap data;
    NMStringMap secrets;

    QVERIFY(Openvpn3Storage::writeSecretProfile(data, secrets, kProfile, NetworkManager::Setting::AgentOwned));

    QCOMPARE(data,
             (NMStringMap{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}}));
    QCOMPARE(secrets.value(u"profile"_s), encoded(kProfile));
    QCOMPARE(Openvpn3Storage::readProfile(data, secrets), kProfile);
    QCOMPARE(Openvpn3Storage::profileFlags(data), std::optional<Flags>(NetworkManager::Setting::AgentOwned));
}

void Openvpn3StorageTest::writesTheSystemLayout()
{
    NMStringMap data;
    NMStringMap secrets;

    QVERIFY(Openvpn3Storage::writeSecretProfile(data, secrets, kProfile, NetworkManager::Setting::None));

    QCOMPARE(data.value(u"profile-flags"_s), u"0"_s);
    QCOMPARE(Openvpn3Storage::profileFlags(data), std::optional<Flags>(NetworkManager::Setting::None));
    QCOMPARE(Openvpn3Storage::readProfile(data, secrets), kProfile);
}

void Openvpn3StorageTest::migratingToSecretRemovesThePublicCopy()
{
    NMStringMap data{{u"profile"_s, encoded(kProfile)}, {u"username"_s, u"testuser"_s}};
    NMStringMap secrets;

    QVERIFY(Openvpn3Storage::writeSecretProfile(data, secrets, kProfile, NetworkManager::Setting::AgentOwned));

    QVERIFY(!data.contains(u"profile"_s));
    QCOMPARE(data.value(u"username"_s), u"testuser"_s); // nothing else disturbed
}

void Openvpn3StorageTest::migratingBackRemovesTheSecret()
{
    NMStringMap data{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}};
    NMStringMap secrets{{u"profile"_s, encoded(kProfile)}, {u"password"_s, u"pw"_s}};

    Openvpn3Storage::writeLegacyProfile(data, secrets, kProfile);

    QVERIFY(!secrets.contains(u"profile"_s));
    QCOMPARE(secrets.value(u"password"_s), u"pw"_s);
    QVERIFY(!Openvpn3Storage::isSecretMode(data));
}

void Openvpn3StorageTest::keyMaterialNeverReachesTheDataMap()
{
    NMStringMap data;
    NMStringMap secrets;

    QVERIFY(Openvpn3Storage::writeSecretProfile(data, secrets, kProfile, NetworkManager::Setting::AgentOwned));

    for (const QString &value : publicValues(data)) {
        QVERIFY(!value.contains(u"VERYSECRET"_s));
        QVERIFY(!value.contains(encoded(kProfile)));
        QVERIFY(!value.contains(u"PRIVATE KEY"_s));
    }
}

void Openvpn3StorageTest::refusesToWriteFlagsThatWouldNotStoreTheProfile()
{
    for (auto flags : {NetworkManager::Setting::NotSaved, NetworkManager::Setting::NotRequired}) {
        NMStringMap data;
        NMStringMap secrets;
        QVERIFY(!Openvpn3Storage::writeSecretProfile(data, secrets, kProfile, flags));
        QVERIFY(data.isEmpty());
        QVERIFY(secrets.isEmpty());
    }
}

void Openvpn3StorageTest::writersDefaultToTheWallet()
{
    NMStringMap data;
    NMStringMap secrets;

    QVERIFY(Openvpn3Storage::writeSecretProfile(data, secrets, kProfile));

    QCOMPARE(data.value(u"profile-flags"_s), u"1"_s);
}

void Openvpn3StorageTest::absentFlagsMeanSystemStorage()
{
    // NetworkManager reads a secret without flags as None, i.e. system-owned.
    // Guessing AgentOwned would move an unattended connection's profile into
    // the user's wallet the first time anything saved the connection.
    const NMStringMap data{{u"profile-storage"_s, u"secret"_s}};
    const NMStringMap secrets{{u"profile"_s, encoded(kProfile)}};

    QCOMPARE(Openvpn3Storage::profileFlags(data), std::optional<Flags>(NetworkManager::Setting::None));
    QCOMPARE(Openvpn3Storage::availability(data, secrets), Openvpn3Storage::Availability::Available);
}

void Openvpn3StorageTest::refusesToReadFlagsThatWouldNotStoreTheProfile_data()
{
    QTest::addColumn<QString>("flags");

    QTest::newRow("not saved") << u"2"_s;
    QTest::newRow("not required") << u"4"_s;
    QTest::newRow("combination") << u"3"_s;
    QTest::newRow("out of range") << u"8"_s;
    QTest::newRow("negative") << u"-1"_s;
    QTest::newRow("not a number") << u"nonsense"_s;
}

void Openvpn3StorageTest::refusesToReadFlagsThatWouldNotStoreTheProfile()
{
    QFETCH(QString, flags);
    const NMStringMap data{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, flags}};
    const NMStringMap secrets{{u"profile"_s, encoded(kProfile)}};

    // The secret is there, but the connection says it is never stored. That
    // cannot be true of a profile, so say so instead of inventing a value.
    QCOMPARE(Openvpn3Storage::profileFlags(data), std::nullopt);
    QCOMPARE(Openvpn3Storage::availability(data, secrets), Openvpn3Storage::Availability::UnusableFlags);
}

void Openvpn3StorageTest::unknownStorageMarkerFailsClosed()
{
    // Written by a newer version of the contract. Whatever is in the data
    // item belongs to the layout this connection used before, so reading it
    // would connect with a stale profile.
    const NMStringMap data{
        {u"profile-storage"_s, u"v2-whatever"_s},
        {u"profile"_s, encoded(kProfile)},
    };
    const NMStringMap secrets{{u"profile"_s, encoded(kProfile)}};

    QVERIFY(!Openvpn3Storage::isSecretMode(data));
    QCOMPARE(Openvpn3Storage::availability(data, secrets), Openvpn3Storage::Availability::Unsupported);
    QVERIFY(Openvpn3Storage::readProfile(data, secrets).isEmpty());
    QCOMPARE(Openvpn3Storage::profileFlags(data), std::nullopt);
}

void Openvpn3StorageTest::emptyStorageMarkerIsTheLegacyLayout()
{
    const NMStringMap data{{u"profile-storage"_s, QString()}, {u"profile"_s, encoded(kProfile)}};

    QCOMPARE(Openvpn3Storage::availability(data, {}), Openvpn3Storage::Availability::Available);
    QCOMPARE(Openvpn3Storage::readProfile(data, {}), kProfile);
}

void Openvpn3StorageTest::clearRemovesEveryTrace()
{
    NMStringMap data{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"1"_s}, {u"profile"_s, u"stale"_s}};
    NMStringMap secrets{{u"profile"_s, encoded(kProfile)}};

    Openvpn3Storage::clearProfile(data, secrets);

    QVERIFY(data.isEmpty());
    QVERIFY(secrets.isEmpty());
    QCOMPARE(Openvpn3Storage::availability(data, secrets), Openvpn3Storage::Availability::Absent);
}

QTEST_GUILESS_MAIN(Openvpn3StorageTest)

#include "openvpn3storagetest.moc"
