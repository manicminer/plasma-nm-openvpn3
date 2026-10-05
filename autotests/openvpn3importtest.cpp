/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <csignal>
#include <sys/resource.h>

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include "nm-openvpn3-service.h"
#include "openvpn3importer.h"
#include "openvpn3profile.h"
#include "openvpn3storage.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
QString dataPath(const QString &name)
{
    return QString::fromLatin1(OPENVPN3_TEST_DATA_DIR) + QLatin1Char('/') + name;
}
}

/**
 * The importer, and what it does to a connection's VPN settings.
 *
 * The parts that go through the openvpn3 backend's own libnm plugin only run
 * when that plugin is installed; everything else is exercised either way.
 */
class Openvpn3ImportTest : public QObject
{
    Q_OBJECT

private:
    //! True when the openvpn3 backend's libnm plugin is available to libnm.
    static bool backendAvailable();

private Q_SLOTS:
    void init() { Openvpn3Storage::setSecretServiceAvailability(true); }

    void scopedCredentialsSurviveApply()
    {
        const auto imported = Openvpn3Import::fromProfileText(u"client\n<connection>\nremote example.org\nauth-user-pass\n</connection>\n"_s, u"alice"_s, u"synthetic"_s);
        NMStringMap data, secrets;
        QVERIFY(Openvpn3Importer::apply(imported, data, secrets));
        QCOMPARE(data.value(u"username"_s), u"alice"_s);
        QCOMPARE(secrets.value(u"password"_s), u"synthetic"_s);
    }
    void realScopedImportPreservesCredentials()
    {
        // This one goes through the real importer, so it skips with the rest
        // of them rather than failing where the backend is not installed.
        if (!backendAvailable()) {
            QSKIP("the openvpn3 backend's libnm plugin is not installed");
        }
        const auto imported = Openvpn3Importer::normalize(u"client\n<connection>\nremote example.org\n<auth-user-pass>\nalice\nsynthetic\n</auth-user-pass>\n</connection>\n"_s);
        QVERIFY2(imported.isValid(), qPrintable(imported.errorMessage()));
        QVERIFY(imported.needsUserPass());
        NMStringMap data, secrets;
        QVERIFY(Openvpn3Importer::apply(imported, data, secrets));
        QCOMPARE(data.value(u"username"_s), u"alice"_s);
        QCOMPARE(secrets.value(u"password"_s), u"synthetic"_s);
    }

    void normalizeRefusesAProfileItCouldNotWriteWhole();
    void shreddingOverwritesRatherThanTruncates();

    void applyProducesTheWalletLayout();
    void applyKeepsAnExplicitSystemChoice();
    void applyKeepsThePasswordStorageChoice();
    void applyLeavesNothingBehindFromTheOldProfile();
    void applyRefusesAnInvalidImport();
    void applyMarksOneTimeCodesAsNeverStored();
    void applyWithoutUserPassStoresNoPasswordKeys();

    void applyStoresThePasswordWhereThePolicySays();
    void applyNeverStoresAPasswordItWasToldNotTo();

    void importInlinesEverythingTheProfileReferred();
    void importLiftsCredentialsOutOfTheProfile();
    void importDropsTheFormattingTheFileHad();
    void importOfAMissingFileChangesNothing();
    void importedConnectionUsesTheSecretLayout();
    void importedCredentialsFollowTheProfileIntoTheWallet();
    void importedCredentialsCanBeAskedForBySystemStorage();

    void normalizeEmbedsFilesAndLiftsCredentials();
    void normalizeLeavesASelfContainedProfileAlone();
    void normalizeRefusesWhatItCannotMakeSelfContained();
    void normalizeKeepsWhatTheFrontendsCommentStrippingLeft();
    void normalizeLeavesNothingBehindOnDisk();
};

bool Openvpn3ImportTest::backendAvailable()
{
    static const bool available = Openvpn3Importer::fromFile(dataPath(u"office.ovpn"_s)).isValid();
    return available;
}

// -- apply(): pure map surgery, no backend needed -----------------------------

void Openvpn3ImportTest::applyProducesTheWalletLayout()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote a.example.org\nauth-user-pass\n"_s, u"alice"_s);
    NMStringMap data;
    NMStringMap secrets;

    QVERIFY(Openvpn3Importer::apply(import, data, secrets));

    QCOMPARE(data.value(u"profile-storage"_s), u"secret"_s);
    QCOMPARE(data.value(u"profile-flags"_s), u"1"_s);
    QVERIFY(!data.contains(u"profile"_s));
    QCOMPARE(Openvpn3Storage::readProfile(data, secrets), import.profile());
    QCOMPARE(data.value(u"username"_s), u"alice"_s);
}

void Openvpn3ImportTest::applyKeepsAnExplicitSystemChoice()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote a.example.org\n"_s);
    NMStringMap data{{u"profile-storage"_s, u"secret"_s}, {u"profile-flags"_s, u"0"_s}};
    NMStringMap secrets{{u"profile"_s, u"b2xk"_s}};

    QVERIFY(Openvpn3Importer::apply(import, data, secrets));

    // Reimport replaces the configuration, not the policy the user chose.
    QCOMPARE(data.value(u"profile-flags"_s), u"0"_s);
}

void Openvpn3ImportTest::applyKeepsThePasswordStorageChoice()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote a.example.org\nauth-user-pass\n"_s);
    NMStringMap data{{u"password-flags"_s, u"2"_s}};
    NMStringMap secrets;

    QVERIFY(Openvpn3Importer::apply(import, data, secrets));

    QCOMPARE(data.value(u"password-flags"_s), u"2"_s);
}

void Openvpn3ImportTest::applyLeavesNothingBehindFromTheOldProfile()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote new.example.org\n"_s);
    NMStringMap data{
        {u"profile"_s, u"b2xk"_s},
        {u"username"_s, u"bob"_s},
        {u"cert-pass-flags"_s, u"1"_s},
        {u"challenge-response-flags"_s, u"2"_s},
    };
    NMStringMap secrets{{u"password"_s, u"old"_s}, {u"cert-pass"_s, u"old"_s}};

    QVERIFY(Openvpn3Importer::apply(import, data, secrets));

    // The new profile needs neither, so neither is carried over.
    QVERIFY(!data.contains(u"username"_s));
    QVERIFY(!data.contains(u"profile"_s));
    QVERIFY(!secrets.contains(u"password"_s));
    QVERIFY(!secrets.contains(u"cert-pass"_s));
}

void Openvpn3ImportTest::applyRefusesAnInvalidImport()
{
    const Openvpn3Import import; // as returned by a failed fromFile()
    NMStringMap data{{u"profile"_s, u"b2xk"_s}};
    NMStringMap secrets{{u"password"_s, u"pw"_s}};
    const NMStringMap dataBefore = data;
    const NMStringMap secretsBefore = secrets;

    QVERIFY(!Openvpn3Importer::apply(import, data, secrets));

    QCOMPARE(data, dataBefore);
    QCOMPARE(secrets, secretsBefore);
}

void Openvpn3ImportTest::applyMarksOneTimeCodesAsNeverStored()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote a.example.org\nauth-user-pass\nstatic-challenge \"PIN\" 1\n"_s);
    NMStringMap data;
    NMStringMap secrets;

    QVERIFY(Openvpn3Importer::apply(import, data, secrets));

    QCOMPARE(data.value(u"challenge-response-flags"_s), QString::number(NetworkManager::Setting::NotSaved));
    QVERIFY(!secrets.contains(u"challenge-response"_s));
}

void Openvpn3ImportTest::applyWithoutUserPassStoresNoPasswordKeys()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote a.example.org\n"_s);
    NMStringMap data;
    NMStringMap secrets;

    QVERIFY(Openvpn3Importer::apply(import, data, secrets));

    QVERIFY(!data.contains(u"password-flags"_s));
    QVERIFY(!data.contains(u"username"_s));
}

void Openvpn3ImportTest::applyStoresThePasswordWhereThePolicySays()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote a.example.org\nauth-user-pass\n"_s, u"alice"_s, u"pw"_s);
    NMStringMap data;
    NMStringMap secrets;

    Openvpn3Policy policy;
    policy.profileFlags = NetworkManager::Setting::None;
    policy.passwordFlags = NetworkManager::Setting::None;
    QVERIFY(Openvpn3Importer::apply(import, data, secrets, policy));

    // The connection was told to keep both for all users, and does.
    QCOMPARE(data.value(u"profile-flags"_s), u"0"_s);
    QCOMPARE(data.value(u"password-flags"_s), u"0"_s);
    QCOMPARE(secrets.value(u"password"_s), u"pw"_s);
}

void Openvpn3ImportTest::applyNeverStoresAPasswordItWasToldNotTo()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote a.example.org\nauth-user-pass\n"_s, u"alice"_s, u"pw"_s);
    NMStringMap data;
    NMStringMap secrets;

    Openvpn3Policy policy;
    policy.passwordFlags = NetworkManager::Setting::NotSaved;
    QVERIFY(Openvpn3Importer::apply(import, data, secrets, policy));

    // "Ask me every time" means the password from the file is not kept, not
    // that it is kept and the flags say otherwise.
    QCOMPARE(data.value(u"password-flags"_s), QString::number(NetworkManager::Setting::NotSaved));
    QVERIFY(!secrets.contains(u"password"_s));
}

/**
 * A profile that could not be written out whole is not normalized from what
 * did get written.
 *
 * The write is made to fail for real rather than mocked: RLIMIT_FSIZE is what
 * a quota or a full filesystem does to it, and the prefix that gets through is
 * a profile the backend would import quite happily, directives and all. If the
 * short write were not noticed, the connection would be saved with the rest of
 * its configuration silently missing.
 */
void Openvpn3ImportTest::normalizeRefusesAProfileItCouldNotWriteWhole()
{
    QString profile = u"client\nremote vpn.example.org 1194 udp\n"_s;
    for (int i = 0; i < 4000; ++i) {
        profile += u"setenv pad%1 value\n"_s.arg(i);
    }
    QVERIFY(profile.size() > 32768);

    rlimit original{};
    QVERIFY(getrlimit(RLIMIT_FSIZE, &original) == 0);
    // Writing past the limit raises SIGXFSZ, which would otherwise kill the
    // test process before the write could return an error.
    auto *previousHandler = signal(SIGXFSZ, SIG_IGN);
    rlimit limited = original;
    limited.rlim_cur = 4096;
    QVERIFY(setrlimit(RLIMIT_FSIZE, &limited) == 0);

    const Openvpn3Import result = Openvpn3Importer::normalize(profile);

    QVERIFY(setrlimit(RLIMIT_FSIZE, &original) == 0);
    signal(SIGXFSZ, previousHandler);

    QVERIFY(!result.isValid());
    QVERIFY(!result.errorMessage().isEmpty());
    QVERIFY(!result.profile().contains(u"setenv pad0 value"_s));
}

/**
 * The overwrite that goes before unlinking a profile has to be an overwrite.
 *
 * Opening the file write-only would truncate it first, which releases the
 * blocks the private key is in and then writes the zeros somewhere else --
 * doing nothing at all, convincingly.
 */
void Openvpn3ImportTest::shreddingOverwritesRatherThanTruncates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"profile.ovpn"_s);
    const QByteArray contents = QByteArray("-----BEGIN PRIVATE KEY-----\nsynthetic\n-----END PRIVATE KEY-----\n");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::NewOnly | QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    }

    QVERIFY(Openvpn3Importer::shredInPlace(path, contents.size()));

    QFile written(path);
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QByteArray after = written.readAll();
    // Same length, so the file was written over where it was rather than cut
    // down to nothing and rebuilt.
    QCOMPARE(after.size(), contents.size());
    QCOMPARE(after, QByteArray(contents.size(), '\0'));

    // Nothing to do for a file that was never written, and not an error.
    QVERIFY(Openvpn3Importer::shredInPlace(path, 0));
    QVERIFY(!Openvpn3Importer::shredInPlace(dir.filePath(u"absent.ovpn"_s), 16));

    // Overwriting a prefix is what tells an overwrite from a truncation
    // followed by a write of the same length: the tail has to still be there.
    const QString partial = dir.filePath(u"partial.ovpn"_s);
    {
        QFile file(partial);
        QVERIFY(file.open(QIODevice::NewOnly | QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    }
    QVERIFY(Openvpn3Importer::shredInPlace(partial, 8));
    QFile tail(partial);
    QVERIFY(tail.open(QIODevice::ReadOnly));
    const QByteArray mixed = tail.readAll();
    QCOMPARE(mixed.size(), contents.size());
    QCOMPARE(mixed.left(8), QByteArray(8, '\0'));
    QCOMPARE(mixed.mid(8), contents.mid(8));
}

// -- the real backend importer ------------------------------------------------

void Openvpn3ImportTest::importInlinesEverythingTheProfileReferred()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    const Openvpn3Import import = Openvpn3Importer::fromFile(dataPath(u"office.ovpn"_s));
    QVERIFY2(import.isValid(), qPrintable(import.errorMessage()));

    const Openvpn3Profile profile = Openvpn3Profile::fromText(import.profile());

    // Nothing points outside the profile any more.
    QVERIFY(!import.profile().contains(u"pki/ca.crt"_s));
    QVERIFY(!import.profile().contains(u"creds.txt"_s));
    QVERIFY(profile.blockBody(u"ca"_s).contains(u"BEGIN CERTIFICATE"_s));
    QVERIFY(profile.blockBody(u"cert"_s).contains(u"BEGIN CERTIFICATE"_s));
    QVERIFY(profile.blockBody(u"key"_s).contains(u"BEGIN PRIVATE KEY"_s));
    QVERIFY(profile.blockBody(u"tls-auth"_s).contains(u"OpenVPN Static key"_s));
    // tls-auth's key direction survives as its own directive.
    QCOMPARE(profile.value(u"key-direction"_s), u"1"_s);
}

void Openvpn3ImportTest::importLiftsCredentialsOutOfTheProfile()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    const Openvpn3Import import = Openvpn3Importer::fromFile(dataPath(u"office.ovpn"_s));
    QVERIFY(import.isValid());

    QCOMPARE(import.username(), u"alice"_s);
    QCOMPARE(import.password(), u"correct-horse-battery-staple"_s);
    QVERIFY(import.needsUserPass());
    QVERIFY(!import.profile().contains(u"correct-horse-battery-staple"_s));
    QCOMPARE(import.suggestedId(), u"office"_s);
}

void Openvpn3ImportTest::importDropsTheFormattingTheFileHad()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    // office.ovpn is a file as people write them: a banner at the top, a note
    // on the failover entry, blank lines spacing the sections out.  openvpn3
    // reads nothing from any of that, and the stored profile is not a file
    // anybody opens again, so the backend's normalizer drops it and the editor
    // has no rows it cannot act on.
    const Openvpn3Import import = Openvpn3Importer::fromFile(dataPath(u"office.ovpn"_s));
    QVERIFY2(import.isValid(), qPrintable(import.errorMessage()));

    const Openvpn3Profile profile = Openvpn3Profile::fromText(import.profile());
    for (const Openvpn3Entry &entry : profile.entries()) {
        QVERIFY(entry.isDirective() || entry.isBlock());
    }
    QVERIFY(!import.profile().contains(u"Synthetic test profile"_s));
    QVERIFY(!import.profile().contains(u"failover entry, kept verbatim"_s));
    // The file has blank lines at the top level, inside and around its
    // sections; none of them survives, and nothing is left before the first
    // directive.
    QVERIFY(!import.profile().contains(u"\n\n"_s));
    QVERIFY(import.profile().startsWith(u"client\n"_s));
    // The directives those comments sat among are all still there, in order.
    QCOMPARE(profile.value(u"dev"_s), u"tun"_s);
    QVERIFY(profile.blockBody(u"connection"_s).contains(u"remote fallback.example.org 1194 udp"_s));
    QCOMPARE(profile.arguments(u"some-directive-we-have-never-heard-of"_s), QStringList({u"1"_s, u"2"_s, u"3"_s}));
    QCOMPARE(profile.arguments(u"verify-x509-name"_s), QStringList({u"C=NO, O=Example Org, CN=vpn.example.org"_s, u"subject"_s}));
    // Parsing what came back is a fixed point: nothing left to drop.
    QCOMPARE(Openvpn3Profile::fromText(import.profile()).toText(), import.profile());
}

void Openvpn3ImportTest::importOfAMissingFileChangesNothing()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    QTemporaryDir dir;
    const Openvpn3Import import = Openvpn3Importer::fromFile(dir.filePath(u"nowhere.ovpn"_s));

    QVERIFY(!import.isValid());
    QVERIFY(!import.errorMessage().isEmpty());

    NMStringMap data{{u"profile"_s, u"b2xk"_s}};
    NMStringMap secrets;
    const NMStringMap before = data;
    QVERIFY(!Openvpn3Importer::apply(import, data, secrets));
    QCOMPARE(data, before);
    QVERIFY(secrets.isEmpty());
}

void Openvpn3ImportTest::importedConnectionUsesTheSecretLayout()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    QString error;
    NMConnection *connection = Openvpn3Importer::importConnection(dataPath(u"office.ovpn"_s), NetworkManager::Setting::AgentOwned, &error);
    QVERIFY2(connection, qPrintable(error));

    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    QVERIFY(s_vpn);
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_service_type(s_vpn)), QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    QVERIFY(!nm_setting_vpn_get_data_item(s_vpn, "profile"));
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_data_item(s_vpn, "profile-storage")), u"secret"_s);
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_data_item(s_vpn, "profile-flags")), u"1"_s);
    QVERIFY(nm_setting_vpn_get_secret(s_vpn, "profile"));

    g_object_unref(connection);
}

void Openvpn3ImportTest::importedCredentialsFollowTheProfileIntoTheWallet()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    QString error;
    NMConnection *connection = Openvpn3Importer::importConnection(dataPath(u"office.ovpn"_s), NetworkManager::Setting::AgentOwned, &error);
    QVERIFY2(connection, qPrintable(error));
    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    QVERIFY(s_vpn);

    // The backend leaves a password it read out of the profile system-owned,
    // because it cannot know what the client wants. Importing into Plasma is
    // asking for the wallet, and that has to reach the password too: a
    // connection half in the wallet and half in NetworkManager's own store is
    // not what the user was offered.
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_secret(s_vpn, "password")), u"correct-horse-battery-staple"_s);
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_data_item(s_vpn, "password-flags")),
             QString::number(NetworkManager::Setting::AgentOwned));
    // The one-time code is still never stored.
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_data_item(s_vpn, "challenge-response-flags")),
             QString::number(NetworkManager::Setting::NotSaved));

    g_object_unref(connection);
}

void Openvpn3ImportTest::importedCredentialsCanBeAskedForBySystemStorage()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    QString error;
    NMConnection *connection = Openvpn3Importer::importConnection(dataPath(u"office.ovpn"_s), NetworkManager::Setting::None, &error);
    QVERIFY2(connection, qPrintable(error));
    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);

    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_data_item(s_vpn, "profile-flags")), u"0"_s);
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_data_item(s_vpn, "password-flags")), u"0"_s);

    g_object_unref(connection);
}

// -- normalising a profile that was written here rather than imported ---------

void Openvpn3ImportTest::normalizeEmbedsFilesAndLiftsCredentials()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    // What someone typing into the Profile Source page would produce: a file
    // reference and a password in the document itself.
    const QString typed = u"client\nremote a.example.org 1194 udp\nca "_s + dataPath(u"pki/ca.crt"_s)
        + u"\n<auth-user-pass>\nbob\nhunter2\n</auth-user-pass>\n"_s;

    const Openvpn3Import result = Openvpn3Importer::normalize(typed);
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));

    const Openvpn3Profile profile = Openvpn3Profile::fromText(result.profile());
    QVERIFY(profile.blockBody(u"ca"_s).contains(u"BEGIN CERTIFICATE"_s));
    QVERIFY(!result.profile().contains(u"pki/ca.crt"_s));
    // The credentials belong with the connection now, and only there.
    QCOMPARE(result.username(), u"bob"_s);
    QCOMPARE(result.password(), u"hunter2"_s);
    QVERIFY(!result.profile().contains(u"hunter2"_s));
    // openvpn3 still has to be told to ask for them.
    QVERIFY(profile.containsOption(u"auth-user-pass"_s));
    QVERIFY(result.needsUserPass());
}

void Openvpn3ImportTest::normalizeLeavesASelfContainedProfileAlone()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    const auto selfContained = u"client\nremote a.example.org 1194 udp\n<ca>\nPEM\n</ca>\nsome-directive 1 2 3\n"_s;

    const Openvpn3Import result = Openvpn3Importer::normalize(selfContained);
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));
    QCOMPARE(result.profile(), selfContained);
}

void Openvpn3ImportTest::normalizeRefusesWhatItCannotMakeSelfContained()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    const Openvpn3Import result = Openvpn3Importer::normalize(u"client\nremote a.example.org\nca /nonexistent/nowhere.crt\n"_s);

    QVERIFY(!result.isValid());
    QVERIFY(!result.errorMessage().isEmpty());
    QVERIFY(result.profile().isEmpty());
}

void Openvpn3ImportTest::normalizeKeepsWhatTheFrontendsCommentStrippingLeft()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    // The two comment strippers run one after the other whenever an edited
    // profile still refers to a file: this class drops the comments as the
    // document is parsed, and the backend drops them again while it inlines.
    // What the first pass deliberately kept the second must not take -- the
    // escaped trailing space is the value, and Unicode whitespace is a
    // literal byte of it.
    const QString typed = u"client\nremote a.example.org 1194 udp\nca "_s + dataPath(u"pki/ca.crt"_s)
        + u"\nsetenv a value\\  # note\nsetenv b value  ; note\n"_s;
    const QString stripped = Openvpn3Profile::fromText(typed).toText();
    QVERIFY(stripped.contains(u"setenv a value\\ \n"_s));
    QVERIFY(stripped.contains(u"setenv b value \n"_s));

    const Openvpn3Import result = Openvpn3Importer::normalize(stripped);
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));

    QVERIFY(result.profile().contains(u"setenv a value\\ \n"_s));
    QVERIFY(!result.profile().contains(u"setenv a value\\\n"_s));
    QVERIFY(result.profile().contains(u"setenv b value \n"_s));
    // And normalizing again is a fixed point, as is parsing what came back.
    QCOMPARE(Openvpn3Importer::normalize(result.profile()).profile(), result.profile());
    QCOMPARE(Openvpn3Profile::fromText(result.profile()).toText(), result.profile());
}

void Openvpn3ImportTest::normalizeLeavesNothingBehindOnDisk()
{
    if (!backendAvailable()) {
        QSKIP("the openvpn3 backend's libnm plugin is not installed");
    }
    const QStringList before = QDir(QDir::tempPath()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);

    const Openvpn3Import result = Openvpn3Importer::normalize(u"client\nremote a.example.org\n<auth-user-pass>\nbob\nhunter2\n</auth-user-pass>\n"_s);
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));
    QCOMPARE(result.password(), u"hunter2"_s);

    // The password went through a file; no file is left holding it.
    QCOMPARE(QDir(QDir::tempPath()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), before);
}

QTEST_MAIN(Openvpn3ImportTest)

#include "openvpn3importtest.moc"
