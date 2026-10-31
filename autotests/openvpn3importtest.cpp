/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <csignal>
#include <sys/resource.h>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include "nm-openvpn3-service.h"
#include "openvpn3backendguard.h"
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

private Q_SLOTS:
    void init() { Openvpn3Storage::setSecretServiceAvailability(true); }

    void scopedCredentialsSurviveApply()
    {
        // The backend's service requires a username, and asks for the
        // password, for an auth-user-pass anywhere outside a payload -- a
        // <connection> block included -- so the connection keeps them.
        const auto imported = Openvpn3Import::fromProfileText(u"client\n<connection>\nremote example.org\nauth-user-pass\n</connection>\n"_s, u"alice"_s, u"synthetic"_s);
        QVERIFY(imported.needsUserPass());
        NMStringMap data, secrets;
        QVERIFY(Openvpn3Importer::apply(imported, data, secrets));
        QCOMPARE(data.value(u"username"_s), u"alice"_s);
        QCOMPARE(secrets.value(u"password"_s), u"synthetic"_s);
    }
    void realScopedImportKeepsTheScopeAsWritten()
    {
        // The backend's import keeps a <connection> scope exactly as written:
        // credentials in one are not lifted out (openvpn3 does not read them
        // from a scope either), so they stay in the profile -- a secret like
        // the rest of it -- and the editor reports them as a caveat. Pinned
        // here so that the documented caveat cannot quietly stop being true.
        OPENVPN3_REQUIRE_BACKEND();
        const QString text = u"client\n<connection>\nremote example.org\n<auth-user-pass>\nalice\nsynthetic\n</auth-user-pass>\n</connection>\n"_s;
        const auto imported = Openvpn3Importer::normalize(text);
        QVERIFY2(imported.isValid(), qPrintable(imported.errorMessage()));
        QCOMPARE(imported.profile(), text);
        QVERIFY(imported.username().isEmpty());
        QVERIFY(imported.password().isEmpty());
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
    void importKeepsTheFormattingTheFileHad();
    void importInlinesEveryTopLevelFileKind();
    void importLeavesConnectionBlocksAndOpaqueTextAlone();
    void importOfAMissingReferencedFileFails();
    void importNormalizesLineWhitespace();
    void importRefusesBlockTagsTheBackendReadsDifferently_data();
    void importRefusesBlockTagsTheBackendReadsDifferently();
    void importOfAMissingFileChangesNothing();
    void importedConnectionUsesTheSecretLayout();
    void importedCredentialsFollowTheProfileIntoTheWallet();
    void importedCredentialsCanBeAskedForBySystemStorage();

    void normalizeEmbedsFilesAndLiftsCredentials();
    void normalizeLeavesASelfContainedProfileAlone();
    void normalizeRefusesWhatItCannotMakeSelfContained();
    void resolveSplicesOnlyTheEntriesThatNeedIt();
    void resolveKeepsTheDocumentsLineTerminator();
    void resolveWritesOneBareAuthUserPass();
    void resolveAggregatesCredentialsLikeTheImport();
    void resolveRefusesAMissingFileAndChangesNothing();
    void resolveRefusesARelativePath();
    void resolveLeavesASelfContainedProfileAlone();
    void normalizeLeavesNothingBehindOnDisk();
};

// -- apply(): pure map surgery, no backend needed -----------------------------

void Openvpn3ImportTest::applyProducesTheWalletLayout()
{
    const auto import = Openvpn3Import::fromProfileText(u"client\nremote a.example.org\nauth-user-pass\n"_s, u"alice"_s);
    NMStringMap data;
    NMStringMap secrets;

    QVERIFY(Openvpn3Importer::apply(import, data, secrets));

    QVERIFY(!data.contains(u"profile-storage"_s));
    QCOMPARE(data.value(u"profile-flags"_s), u"1"_s);
    QCOMPARE(data.value(u"remote"_s), u"a.example.org"_s);
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
    OPENVPN3_REQUIRE_BACKEND();
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
    OPENVPN3_REQUIRE_BACKEND();
    const Openvpn3Import import = Openvpn3Importer::fromFile(dataPath(u"office.ovpn"_s));
    QVERIFY(import.isValid());

    QCOMPARE(import.username(), u"alice"_s);
    QCOMPARE(import.password(), u"correct-horse-battery-staple"_s);
    QVERIFY(import.needsUserPass());
    QVERIFY(!import.profile().contains(u"correct-horse-battery-staple"_s));
    QCOMPARE(import.suggestedId(), u"office"_s);
}

void Openvpn3ImportTest::importKeepsTheFormattingTheFileHad()
{
    OPENVPN3_REQUIRE_BACKEND();
    // office.ovpn is a file as people write them: a banner at the top, a note
    // on the failover entry, blank lines spacing the sections out. The
    // backend keeps all of it (docs/connection-format.md: "comments and blank
    // lines included, is kept as written"), and so does this editor.
    const Openvpn3Import import = Openvpn3Importer::fromFile(dataPath(u"office.ovpn"_s));
    QVERIFY2(import.isValid(), qPrintable(import.errorMessage()));

    QVERIFY(import.profile().startsWith(u"##\n# Synthetic test profile for example.org - not a real VPN.\n##\nclient\n"_s));
    QVERIFY(import.profile().contains(u"\n\n; failover entry, kept verbatim by the editor\n<connection>\n"_s));
    QVERIFY(import.profile().contains(u"<connection>\nremote fallback.example.org 1194 udp\nhttp-proxy proxy.example.org 8080\n</connection>\n"_s));

    // The entries are the directives and blocks, in order; the formatting is
    // carried by them rather than being entries of its own.
    const Openvpn3Profile profile = Openvpn3Profile::fromText(import.profile());
    for (const Openvpn3Entry &entry : profile.entries()) {
        QVERIFY(entry.isDirective() || entry.isBlock());
    }
    QCOMPARE(profile.at(0).name, u"client"_s);
    QCOMPARE(profile.value(u"dev"_s), u"tun"_s);
    QCOMPARE(profile.arguments(u"some-directive-we-have-never-heard-of"_s), QStringList({u"1"_s, u"2"_s, u"3"_s}));
    QCOMPARE(profile.arguments(u"verify-x509-name"_s), QStringList({u"C=NO, O=Example Org, CN=vpn.example.org"_s, u"subject"_s}));
    // Reading what came back and writing it out again changes nothing.
    QCOMPARE(profile.toText(), import.profile());
}

namespace
{
/** Writes @p contents to @p name inside @p dir, creating directories. */
bool writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &contents)
{
    const QString path = dir.filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readData(const QString &name)
{
    QFile file(dataPath(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList namesOf(const Openvpn3Profile &profile)
{
    QStringList names;
    for (const Openvpn3Entry &entry : profile.entries()) {
        names.append(entry.isBlock() ? u'<' + entry.name + u'>' : entry.name);
    }
    return names;
}
}

void Openvpn3ImportTest::importInlinesEveryTopLevelFileKind()
{
    OPENVPN3_REQUIRE_BACKEND();
    // Every reference the backend inlines at the top level, a quoted relative
    // path with a space in it among them, against files that exist.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pkcs12("\x30\x82\x01\x00synthetic\x00\xff pkcs12 bytes", 26);
    QVERIFY(writeFile(dir, u"my ca.crt"_s, readData(u"pki/ca.crt"_s)));
    QVERIFY(writeFile(dir, u"pki/client.crt"_s, readData(u"pki/client.crt"_s)));
    QVERIFY(writeFile(dir, u"pki/client.key"_s, readData(u"pki/client.key"_s)));
    QVERIFY(writeFile(dir, u"pki/ta.key"_s, readData(u"pki/ta.key"_s)));
    QVERIFY(writeFile(dir, u"extra.crt"_s, "-----BEGIN CERTIFICATE-----\nEXTRA\n-----END CERTIFICATE-----\n"));
    QVERIFY(writeFile(dir, u"tc.key"_s, "-----BEGIN OpenVPN Static key V1-----\nTLSCRYPT\n-----END OpenVPN Static key V1-----\n"));
    QVERIFY(writeFile(dir, u"tc2.key"_s, "-----BEGIN OpenVPN tls-crypt-v2 client key-----\nTLSCRYPTV2\n-----END OpenVPN tls-crypt-v2 client key-----\n"));
    QVERIFY(writeFile(dir, u"crl.pem"_s, "-----BEGIN X509 CRL-----\nCRL\n-----END X509 CRL-----"));
    QVERIFY(writeFile(dir, u"client.p12"_s, pkcs12));
    QVERIFY(writeFile(dir, u"creds.txt"_s, "carol\ns3cret\n"));
    QVERIFY(writeFile(dir,
                      u"all.ovpn"_s,
                      "client\n"
                      "remote vpn.example.org 1194 udp\n"
                      "ca \"my ca.crt\"\n"
                      "cert pki/client.crt\n"
                      "key pki/client.key\n"
                      "extra-certs extra.crt\n"
                      "tls-auth pki/ta.key 1\n"
                      "tls-crypt tc.key\n"
                      "tls-crypt-v2 tc2.key\n"
                      "crl-verify crl.pem\n"
                      "crl-verify crls dir\n"
                      "pkcs12 client.p12\n"
                      "auth-user-pass creds.txt\n"
                      "setenv dup 1\n"
                      "setenv dup 2\n"
                      "unknown-option a b\n"));

    const Openvpn3Import import = Openvpn3Importer::fromFile(dir.filePath(u"all.ovpn"_s));
    QVERIFY2(import.isValid(), qPrintable(import.errorMessage()));
    const Openvpn3Profile profile = Openvpn3Profile::fromText(import.profile());

    // In place, in order, nothing hoisted or reordered; tls-auth's direction
    // becomes key-direction right behind its block; a CRL directory is a
    // directory, which stays a reference.
    QCOMPARE(namesOf(profile),
             QStringList({u"client"_s, u"remote"_s, u"<ca>"_s, u"<cert>"_s, u"<key>"_s, u"<extra-certs>"_s, u"<tls-auth>"_s, u"key-direction"_s,
                          u"<tls-crypt>"_s, u"<tls-crypt-v2>"_s, u"<crl-verify>"_s, u"crl-verify"_s, u"<pkcs12>"_s, u"auth-user-pass"_s,
                          u"setenv"_s, u"setenv"_s, u"unknown-option"_s}));
    QCOMPARE(profile.blockBody(u"ca"_s), QString::fromUtf8(readData(u"pki/ca.crt"_s)));
    QCOMPARE(profile.blockBody(u"extra-certs"_s), u"-----BEGIN CERTIFICATE-----\nEXTRA\n-----END CERTIFICATE-----\n"_s);
    QVERIFY(profile.blockBody(u"tls-crypt-v2"_s).contains(u"TLSCRYPTV2"_s));
    // A file with no final newline still makes a well-formed block.
    QCOMPARE(profile.blockBody(u"crl-verify"_s), u"-----BEGIN X509 CRL-----\nCRL\n-----END X509 CRL-----\n"_s);
    QCOMPARE(profile.value(u"key-direction"_s), u"1"_s);
    QCOMPARE(profile.at(profile.indexesOf(u"crl-verify"_s).last()).arguments, QStringList({u"crls"_s, u"dir"_s}));
    // A PKCS#12 bundle is binary, so it goes in as base64 and its passphrase
    // is asked for.
    QCOMPARE(profile.blockBody(u"pkcs12"_s).remove(u'\n'), QString::fromLatin1(pkcs12.toBase64()));
    QVERIFY(import.needsCertPass());
    // The credentials file's contents move to the connection; openvpn3 is
    // still told to ask.
    QCOMPARE(import.username(), u"carol"_s);
    QCOMPARE(import.password(), u"s3cret"_s);
    QVERIFY(import.needsUserPass());
    QVERIFY(!import.profile().contains(u"s3cret"_s));
    QVERIFY(!import.profile().contains(u"creds.txt"_s));
}

void Openvpn3ImportTest::importLeavesConnectionBlocksAndOpaqueTextAlone()
{
    OPENVPN3_REQUIRE_BACKEND();
    // Text that reads like a file reference and is not one: a comment, the
    // payload of an unknown block, and a reference inside a <connection>
    // scope, which the backend keeps exactly as written and openvpn3 does
    // not read. None of the files exist, and the import still succeeds:
    // nothing went looking for them.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray text =
        "client\n"
        "# ca commented-out.crt\n"
        "<x-notes>\n"
        "ca not-a-reference.crt\n"
        "</x-notes>\n"
        "<connection>\n"
        "remote fallback.example.org 443 tcp\n"
        "ca scoped-missing.crt\n"
        "</connection>\n"
        "remote vpn.example.org";
    QVERIFY(writeFile(dir, u"opaque.ovpn"_s, text));

    const Openvpn3Import import = Openvpn3Importer::fromFile(dir.filePath(u"opaque.ovpn"_s));
    QVERIFY2(import.isValid(), qPrintable(import.errorMessage()));
    // Byte for byte but for the final newline the backend writes.
    QCOMPARE(import.profile(), QString::fromUtf8(text) + u'\n');
}

void Openvpn3ImportTest::importOfAMissingReferencedFileFails()
{
    OPENVPN3_REQUIRE_BACKEND();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir, u"missing.ovpn"_s, "client\nremote vpn.example.org\nca missing-ca.crt\n"));

    const Openvpn3Import import = Openvpn3Importer::fromFile(dir.filePath(u"missing.ovpn"_s));
    QVERIFY(!import.isValid());
    QVERIFY2(import.errorMessage().contains(u"missing-ca.crt"_s), qPrintable(import.errorMessage()));
}

void Openvpn3ImportTest::importRefusesBlockTagsTheBackendReadsDifferently_data()
{
    QTest::addColumn<QByteArray>("text");
    QTest::newRow("commented connection opener")
        << QByteArray("client\n<connection> # primary\nremote vpn.example.org\n<auth-user-pass>\nalice\npassword\n</auth-user-pass>\n</connection>\n");
    QTest::newRow("quoted connection opener") << QByteArray("client\n\"<connection>\"\nremote vpn.example.org\nca ca.crt\n</connection>\n");
    QTest::newRow("indented closer in a payload") << QByteArray("client\nremote vpn.example.org\n<x-notes>\n </x-notes>\nca ca.crt\n</x-notes>\n");
}

void Openvpn3ImportTest::importRefusesBlockTagsTheBackendReadsDifferently()
{
    OPENVPN3_REQUIRE_BACKEND();
    // openvpn3 reads these as a scope or a payload; the backend's importer,
    // which matches the literal stripped line, does not, and would inline the
    // file or lift the credentials out of it. Refused, on both import paths,
    // rather than silently hoisted -- even with the file right there.
    QFETCH(QByteArray, text);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir, u"ca.crt"_s, readData(u"pki/ca.crt"_s)));
    QVERIFY(writeFile(dir, u"odd.ovpn"_s, text));

    const Openvpn3Import import = Openvpn3Importer::fromFile(dir.filePath(u"odd.ovpn"_s));
    QVERIFY(!import.isValid());
    QVERIFY2(import.errorMessage().contains(u"line of its own"_s), qPrintable(import.errorMessage()));

    QString error;
    NMConnection *connection = Openvpn3Importer::importConnection(dir.filePath(u"odd.ovpn"_s), NetworkManager::Setting::None, &error);
    QVERIFY(!connection);
    QVERIFY2(error.contains(u"line of its own"_s), qPrintable(error));
}

void Openvpn3ImportTest::importNormalizesLineWhitespace()
{
    OPENVPN3_REQUIRE_BACKEND();
    // What the backend's import does to text, pinned so that the documented
    // caveat stays true: every line outside a payload is stripped at both
    // ends -- an escaped trailing space included -- a payload line loses its
    // trailing whitespace, and a comment after a reference it inlines is
    // gone with the reference. This is the import's contract, not something
    // the editor does to a document it already holds.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir, u"ca.crt"_s, readData(u"pki/ca.crt"_s)));
    QVERIFY(writeFile(dir,
                      u"ws.ovpn"_s,
                      "client\n"
                      "  remote vpn.example.org   \n"
                      "setenv a value\\ \n"
                      "ca ca.crt # company CA\n"
                      "<x-blob>\n"
                      "payload   \n"
                      "</x-blob>\n"));

    const Openvpn3Import import = Openvpn3Importer::fromFile(dir.filePath(u"ws.ovpn"_s));
    QVERIFY2(import.isValid(), qPrintable(import.errorMessage()));
    QVERIFY(import.profile().contains(u"\nremote vpn.example.org\n"_s));
    QVERIFY(import.profile().contains(u"\nsetenv a value\\\n"_s));
    QVERIFY(!import.profile().contains(u"company CA"_s));
    QVERIFY(import.profile().contains(u"<x-blob>\npayload\n</x-blob>\n"_s));
}

void Openvpn3ImportTest::importOfAMissingFileChangesNothing()
{
    OPENVPN3_REQUIRE_BACKEND();
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
    OPENVPN3_REQUIRE_BACKEND();
    QString error;
    NMConnection *connection = Openvpn3Importer::importConnection(dataPath(u"office.ovpn"_s), NetworkManager::Setting::AgentOwned, &error);
    QVERIFY2(connection, qPrintable(error));

    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    QVERIFY(s_vpn);
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_service_type(s_vpn)), QLatin1String(NM_DBUS_SERVICE_OPENVPN3));
    QVERIFY(!nm_setting_vpn_get_data_item(s_vpn, "profile"));
    // The backend's layout: the flags are the only marker.
    QVERIFY(!nm_setting_vpn_get_data_item(s_vpn, "profile-storage"));
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_data_item(s_vpn, "profile-flags")), u"1"_s);
    QCOMPARE(QString::fromUtf8(nm_setting_vpn_get_data_item(s_vpn, "remote")), u"vpn1.example.org"_s);
    QVERIFY(nm_setting_vpn_get_secret(s_vpn, "profile"));
    // And it is still the profile the backend imported, comments and all.
    NMStringMap data;
    NMStringMap secrets;
    data.insert(u"profile-flags"_s, u"1"_s);
    secrets.insert(u"profile"_s, QString::fromUtf8(nm_setting_vpn_get_secret(s_vpn, "profile")));
    QVERIFY(Openvpn3Storage::readProfile(data, secrets).contains(u"; failover entry, kept verbatim by the editor\n"_s));

    g_object_unref(connection);
}

void Openvpn3ImportTest::importedCredentialsFollowTheProfileIntoTheWallet()
{
    OPENVPN3_REQUIRE_BACKEND();
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
    OPENVPN3_REQUIRE_BACKEND();
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
    OPENVPN3_REQUIRE_BACKEND();
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
    OPENVPN3_REQUIRE_BACKEND();
    const auto selfContained = u"client\nremote a.example.org 1194 udp\n<ca>\nPEM\n</ca>\nsome-directive 1 2 3\n"_s;

    const Openvpn3Import result = Openvpn3Importer::normalize(selfContained);
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));
    QCOMPARE(result.profile(), selfContained);
}

void Openvpn3ImportTest::normalizeRefusesWhatItCannotMakeSelfContained()
{
    OPENVPN3_REQUIRE_BACKEND();
    const Openvpn3Import result = Openvpn3Importer::normalize(u"client\nremote a.example.org\nca /nonexistent/nowhere.crt\n"_s);

    QVERIFY(!result.isValid());
    QVERIFY(!result.errorMessage().isEmpty());
    QVERIFY(result.profile().isEmpty());
}

// -- resolving the references of a document the editor holds ----------------

void Openvpn3ImportTest::resolveSplicesOnlyTheEntriesThatNeedIt()
{
    OPENVPN3_REQUIRE_BACKEND();
    // A document typed or edited here. Only the reference and the inline
    // credentials go through the backend, one entry at a time; everything
    // around them -- the formatting, an escaped trailing space the backend
    // would strip, a scope it would leave alone anyway -- is not its to
    // touch, and comes back byte for byte.
    const QString ca = dataPath(u"pki/ca.crt"_s);
    const QString head = u"# typed here\nclient\nremote a.example.org 1194 udp   # primary\n  setenv a value\\  # escaped space\n"_s;
    const QString tail = u"\n<connection>\nremote b.example.org\nca scoped.crt\n</connection>\n"_s;
    const QString typed = head + u"ca "_s + ca + u"   # company CA\n"_s + tail + u"<auth-user-pass>\nbob\nhunter2\n</auth-user-pass>\ndev tun"_s;

    const Openvpn3Import result = Openvpn3Importer::resolveReferences(Openvpn3Profile::fromText(typed));
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));

    const QString expected = head + u"# company CA\n<ca>\n"_s + QString::fromUtf8(readData(u"pki/ca.crt"_s)) + u"</ca>\n"_s + tail
        + u"auth-user-pass\ndev tun"_s;
    QCOMPARE(result.profile(), expected);
    QCOMPARE(result.username(), u"bob"_s);
    QCOMPARE(result.password(), u"hunter2"_s);
    QVERIFY(result.needsUserPass());
}

void Openvpn3ImportTest::resolveKeepsTheDocumentsLineTerminator()
{
    OPENVPN3_REQUIRE_BACKEND();
    // Both an LF file and a CRLF one come out with CRLF lines, and nothing
    // gets a second carriage return.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir, u"crlf.crt"_s, "-----BEGIN CERTIFICATE-----\r\nCRLF\r\n-----END CERTIFICATE-----\r\n"));
    const QString typed = u"client\r\nremote a.example.org\r\nca "_s + dataPath(u"pki/ca.crt"_s) + u"\r\ncert "_s + dir.filePath(u"crlf.crt"_s) + u"\r\n"_s;

    const Openvpn3Import result = Openvpn3Importer::resolveReferences(Openvpn3Profile::fromText(typed));
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));
    QVERIFY(result.profile().startsWith(u"client\r\nremote a.example.org\r\n<ca>\r\n"_s));
    QVERIFY(result.profile().endsWith(u"<cert>\r\n-----BEGIN CERTIFICATE-----\r\nCRLF\r\n-----END CERTIFICATE-----\r\n</cert>\r\n"_s));
    QVERIFY(!result.profile().contains(QRegularExpression(u"[^\r]\n"_s)));
    QVERIFY(!result.profile().contains(u"\r\r"_s));
}

void Openvpn3ImportTest::resolveWritesOneBareAuthUserPass()
{
    OPENVPN3_REQUIRE_BACKEND();
    // The backend writes the directive once whatever the credentials came
    // in as; a second one, from the inline block, is not added next to it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir, u"creds.txt"_s, "carol\nfile-pw\n"));
    const Openvpn3Import result = Openvpn3Importer::resolveReferences(Openvpn3Profile::fromText(
        u"client\nremote a.example.org\nauth-user-pass\n# creds\n<auth-user-pass>\nbob\npw\n</auth-user-pass>\nverb 3\nauth-user-pass "_s
        + dir.filePath(u"creds.txt"_s) + u" # account note\n"_s));
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));
    // The comment on a collapsed source stays, as a line of its own.
    QCOMPARE(result.profile(), u"client\nremote a.example.org\nauth-user-pass\n# creds\nverb 3\n# account note\n"_s);
    QCOMPARE(result.username(), u"carol"_s);
    QCOMPARE(result.password(), u"file-pw"_s);
}

void Openvpn3ImportTest::resolveAggregatesCredentialsLikeTheImport()
{
    OPENVPN3_REQUIRE_BACKEND();
    // Two credential sources: the backend keeps a username or password from
    // an earlier one when a later one has none. Resolving the same document
    // entry by entry has to come out the same as importing it whole.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir, u"first.txt"_s, "alice\nsecret\n"));
    QVERIFY(writeFile(dir, u"second.txt"_s, "bob\n\n"));
    const QString text = u"client\nremote a.example.org\nauth-user-pass "_s + dir.filePath(u"first.txt"_s) + u"\nauth-user-pass "_s
        + dir.filePath(u"second.txt"_s) + u"\n"_s;
    QVERIFY(writeFile(dir, u"two.ovpn"_s, text.toUtf8()));

    const Openvpn3Import whole = Openvpn3Importer::fromFile(dir.filePath(u"two.ovpn"_s));
    QVERIFY2(whole.isValid(), qPrintable(whole.errorMessage()));
    const Openvpn3Import resolved = Openvpn3Importer::resolveReferences(Openvpn3Profile::fromText(text));
    QVERIFY2(resolved.isValid(), qPrintable(resolved.errorMessage()));
    QCOMPARE(resolved.username(), whole.username());
    QCOMPARE(resolved.password(), whole.password());
    QCOMPARE(resolved.username(), u"bob"_s);
    QCOMPARE(resolved.password(), u"secret"_s);
}

void Openvpn3ImportTest::resolveRefusesAMissingFileAndChangesNothing()
{
    OPENVPN3_REQUIRE_BACKEND();
    const Openvpn3Import result =
        Openvpn3Importer::resolveReferences(Openvpn3Profile::fromText(u"client\nremote a.example.org\nca /nonexistent/nowhere.crt\n"_s));

    QVERIFY(!result.isValid());
    QVERIFY2(result.errorMessage().contains(u"/nonexistent/nowhere.crt"_s), qPrintable(result.errorMessage()));
    QVERIFY(result.profile().isEmpty());
}

void Openvpn3ImportTest::resolveRefusesARelativePath()
{
    // A document held here has no directory a relative path could be read
    // against; the backend would resolve it against a temporary directory and
    // report a path the user never wrote. Say what is wrong instead. This
    // needs no backend: nothing is read.
    const Openvpn3Import result = Openvpn3Importer::resolveReferences(Openvpn3Profile::fromText(u"client\nremote a.example.org\ncert pki/client.crt\n"_s));

    QVERIFY(!result.isValid());
    QVERIFY2(result.errorMessage().contains(u"pki/client.crt"_s), qPrintable(result.errorMessage()));
    QVERIFY2(result.errorMessage().contains(u"relative"_s), qPrintable(result.errorMessage()));
}

void Openvpn3ImportTest::resolveLeavesASelfContainedProfileAlone()
{
    // Nothing to resolve, so nothing is asked of the backend and the text is
    // exactly what was given -- including what an import would normalize.
    const QString text = u"# note\nclient\n  remote a.example.org   ; trailing\n<connection>\nremote b.example.org\nca scoped.crt\n</connection>\n<x>\n  payload  \n</x>"_s;

    const Openvpn3Import result = Openvpn3Importer::resolveReferences(Openvpn3Profile::fromText(text));
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));
    QCOMPARE(result.profile(), text);
}

void Openvpn3ImportTest::normalizeLeavesNothingBehindOnDisk()
{
    OPENVPN3_REQUIRE_BACKEND();
    const QStringList before = QDir(QDir::tempPath()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);

    const Openvpn3Import result = Openvpn3Importer::normalize(u"client\nremote a.example.org\n<auth-user-pass>\nbob\nhunter2\n</auth-user-pass>\n"_s);
    QVERIFY2(result.isValid(), qPrintable(result.errorMessage()));
    QCOMPARE(result.password(), u"hunter2"_s);

    // The password went through a file; no file is left holding it.
    QCOMPARE(QDir(QDir::tempPath()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), before);
}

QTEST_MAIN(Openvpn3ImportTest)

#include "openvpn3importtest.moc"
