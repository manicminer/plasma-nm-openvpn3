/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <QSet>
#include <QTest>

#include "openvpn3profile.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
/** A profile as a file on disk has it: comments on lines of their own, after
 * directives, inside a @c <connection> scope, blank lines spacing the sections
 * out, and characters that look like a comment but are part of a value or of a
 * payload. */
const auto kRich = QStringLiteral(
    "##\n"
    "# An office profile\n"
    "##\n"
    "client\n"
    "dev tun # the device\n"
    "proto udp\n"
    "remote vpn1.example.net 1194 udp\n"
    "remote vpn2.example.net 443 tcp\t; the fallback\n"
    "remote vpn1.example.net 1194 udp\n"
    "\n"
    "; a semicolon comment\n"
    "auth-user-pass\n"
    "pull-filter ignore \"redirect-gateway\"\n"
    "setenv opt 'single quoted value'\n"
    "setenv hash \"a # inside quotes\"\n"
    "verify-x509-name \"C=NO, O=Example, CN=server\" subject\n"
    "<connection>\n"
    "# the failover entry\n"
    "\n"
    "remote fallback.example.net 1194 udp\n"
    "http-proxy proxy.example.net 8080 # via the proxy\n"
    "</connection>\n"
    "<ca>\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBsyntheticTESTDATA # payload, not a comment\n"
    "\n"
    "-----END CERTIFICATE-----\n"
    "</ca>\n"
    "key-direction 1\n"
    "some-directive-we-have-never-heard-of 1 2 3\n");

/** kRich as this class keeps it: the formatting -- the comments and the blank
 * lines -- is gone and nothing else is. Order and duplicates stay, quoting
 * stays, and the lines of the opaque @c <ca> payload, the blank one included,
 * are untouched. */
const auto kRichKept = QStringLiteral(
    "client\n"
    "dev tun\n"
    "proto udp\n"
    "remote vpn1.example.net 1194 udp\n"
    "remote vpn2.example.net 443 tcp\n"
    "remote vpn1.example.net 1194 udp\n"
    "auth-user-pass\n"
    "pull-filter ignore \"redirect-gateway\"\n"
    "setenv opt 'single quoted value'\n"
    "setenv hash \"a # inside quotes\"\n"
    "verify-x509-name \"C=NO, O=Example, CN=server\" subject\n"
    "<connection>\n"
    "remote fallback.example.net 1194 udp\n"
    "http-proxy proxy.example.net 8080\n"
    "</connection>\n"
    "<ca>\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBsyntheticTESTDATA # payload, not a comment\n"
    "\n"
    "-----END CERTIFICATE-----\n"
    "</ca>\n"
    "key-direction 1\n"
    "some-directive-we-have-never-heard-of 1 2 3\n");
}

class Openvpn3ProfileTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void unterminatedEntryBoundaries_data()
    {
        QTest::addColumn<QString>("newline");
        QTest::addColumn<bool>("move");
        QTest::newRow("append-lf") << u"\n"_s << false;
        QTest::newRow("append-crlf") << u"\r\n"_s << false;
        QTest::newRow("move-lf") << u"\n"_s << true;
        QTest::newRow("move-crlf") << u"\r\n"_s << true;
    }
    void unterminatedEntryBoundaries()
    {
        QFETCH(QString, newline);
        QFETCH(bool, move);
        const QString original = u"client"_s + newline + u"remote first.example.org"_s;
        auto profile = Openvpn3Profile::fromText(original);
        QCOMPARE(profile.toText(), original);
        if (move) {
            profile.move(0, 1);
            QCOMPARE(profile.toText(), u"remote first.example.org"_s + newline + u"client"_s + newline);
        } else {
            profile.append(Openvpn3Profile::directive(u"remote"_s, {u"second.example.org"_s}));
            QVERIFY(profile.toText().startsWith(original + newline + u"remote second.example.org"_s));
            QCOMPARE(Openvpn3Profile::fromText(profile.toText()).remoteHosts().size(), 2);
        }
    }

    void roundTripKeepsEverythingButFormatting_data();
    void roundTripKeepsEverythingButFormatting();
    void commentsAreDropped_data();
    void commentsAreDropped();
    void blankLinesAreDropped_data();
    void blankLinesAreDropped();
    void noEntryIsEverFormatting();
    void aScopeBodyNeverTakesAComment();
    void aScopeBodyNeverTakesABlankLine();
    void noMutatorCanPutABlankLineBack();
    void aMalformedCloserNeverBecomesAScopeBoundary();
    void parsesEntryKinds();
    void keepsDuplicatesInOrder();
    void keepsBlocksVerbatim();
    void doesNotDescendIntoConnectionBlocks();
    void parsesQuoting_data();
    void parsesQuoting();
    void anEscapedSeparatorIsPartOfItsValue();
    void anEscapedTrailingSeparatorIsPartOfItsValue_data();
    void anEscapedTrailingSeparatorIsPartOfItsValue();
    void untouchedEntriesKeepTheirQuoting();
    void editingOneEntryLeavesTheOthersAlone();
    void editingKeepsArgumentsItWasNotToldAbout();
    void setDirectiveAppendsWhenMissing();
    void setPresentKeepsExistingArguments();
    void removeAllRemovesEveryDuplicate();
    void blocksCanBeReplacedAndAdded();
    void entriesCanBeInsertedMovedAndRemoved();
    void quotingRoundTrips_data();
    void quotingRoundTrips();
    void editingKeepsAUnicodeWhitespaceValue();
    void unterminatedBlockIsKeptVerbatim();
    void spotsKeyMaterialThatCouldNeedAPassphrase_data();
    void spotsKeyMaterialThatCouldNeedAPassphrase();

    void optionsAreFoundInsideConnectionBlocks();
    void opaqueBlocksAreNeverDescendedInto();
    void nestedInlineCredentialsAreFoundAsABlock();
    void remoteHostsComeFromEveryScope_data();
    void remoteHostsComeFromEveryScope();
    void normalizationIsNeededOnlyForFilesAndCredentials_data();
    void normalizationIsNeededOnlyForFilesAndCredentials();
    void entriesKeepTheirIdentityWhileTheDocumentChanges();
    void parsedEntriesHaveDistinctIdentities();
    void crlfIsVisibleAsTheDocumentsConvention_data();
    void crlfIsVisibleAsTheDocumentsConvention();
};

void Openvpn3ProfileTest::roundTripKeepsEverythingButFormatting_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");

    QTest::newRow("rich") << kRich << kRichKept;
    QTest::newRow("empty") << QString() << QString();
    QTest::newRow("no trailing newline") << u"client\nremote a.example.net 1194"_s << u"client\nremote a.example.net 1194"_s;
    QTest::newRow("crlf") << u"client\r\nremote a.example.net 1194\r\n<ca>\r\nPEM\r\n</ca>\r\n"_s
                          << u"client\r\nremote a.example.net 1194\r\n<ca>\r\nPEM\r\n</ca>\r\n"_s;
    // The whitespace around a directive is part of the line and stays; a line
    // that is nothing but whitespace is not a line of the document at all.
    QTest::newRow("odd whitespace") << u"  client   \n\t remote   a.example.net    1194  \n\n\n"_s
                                    << u"  client   \n\t remote   a.example.net    1194  \n"_s;
    QTest::newRow("block without trailing newline") << u"client\n<ca>\nPEM\n</ca>"_s << u"client\n<ca>\nPEM\n</ca>"_s;
    // Nothing but formatting is nothing at all, and an empty document is one
    // this class can hand back.
    QTest::newRow("only comments") << u"# one\n; two\n"_s << QString();
    QTest::newRow("only comments, crlf") << u"# one\r\n; two\r\n"_s << QString();
    QTest::newRow("only blank lines") << u"\n\n   \n\t\n"_s << QString();
    QTest::newRow("only blank lines, crlf") << u"\r\n\r\n"_s << QString();
    QTest::newRow("only formatting") << u"\n# one\n\n; two\n   \n"_s << QString();
}

void Openvpn3ProfileTest::roundTripKeepsEverythingButFormatting()
{
    QFETCH(QString, text);
    QFETCH(QString, expected);

    const Openvpn3Profile profile = Openvpn3Profile::fromText(text);
    QCOMPARE(profile.toText(), expected);
    // And doing it again changes nothing more.
    QCOMPARE(Openvpn3Profile::fromText(profile.toText()).toText(), expected);
}

void Openvpn3ProfileTest::commentsAreDropped_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");

    // -- what goes --
    QTest::newRow("hash line") << u"# a comment\nclient\n"_s << u"client\n"_s;
    QTest::newRow("semicolon line") << u"; a comment\nclient\n"_s << u"client\n"_s;
    QTest::newRow("indented line") << u"client\n    # indented\n"_s << u"client\n"_s;
    QTest::newRow("hash on a hash") << u"## a banner\nclient\n"_s << u"client\n"_s;
    QTest::newRow("after a directive") << u"dev tun # the device\n"_s << u"dev tun\n"_s;
    QTest::newRow("after a tab") << u"dev tun\t; the device\n"_s << u"dev tun\n"_s;
    QTest::newRow("no space after the hash") << u"dev tun #x\n"_s << u"dev tun\n"_s;
    QTest::newRow("comment is the whole last line, unterminated") << u"client\n# trailing"_s << u"client\n"_s;
    QTest::newRow("inline on the last line, unterminated") << u"client\ndev tun # why"_s << u"client\ndev tun"_s;
    QTest::newRow("crlf") << u"# gone\r\nclient\r\ndev tun # gone\r\n"_s << u"client\r\ndev tun\r\n"_s;
    QTest::newRow("inside a connection scope") << u"<connection>\n# gone\nremote a.example.net # gone\n</connection>\n"_s
                                               << u"<connection>\nremote a.example.net\n</connection>\n"_s;
    QTest::newRow("on an opening tag line") << u"<ca> # the CA\nPEM\n</ca>\n"_s << u"<ca>\nPEM\n</ca>\n"_s;

    // -- what stays --
    QTest::newRow("inside double quotes") << u"setenv a \"# value\"\n"_s << u"setenv a \"# value\"\n"_s;
    QTest::newRow("inside single quotes") << u"setenv a '; value'\n"_s << u"setenv a '; value'\n"_s;
    QTest::newRow("escaped") << u"setenv a \\#value\n"_s << u"setenv a \\#value\n"_s;
    // OpenVPN 2 reads this as the literal value a#b, openvpn3 stops at the
    // hash. They disagree, so it is not a comment anybody can be sure about.
    QTest::newRow("glued to a word") << u"setenv a value#glued\n"_s << u"setenv a value#glued\n"_s;
    QTest::newRow("opaque payload") << u"<ca>\n# payload\nA;B#C\n</ca>\n"_s << u"<ca>\n# payload\nA;B#C\n</ca>\n"_s;
    // A credential is payload too, and one may well start with a '#' or a
    // ';'. Taking it for a comment would hand openvpn3 the wrong password.
    QTest::newRow("opaque credentials") << u"<auth-user-pass>\n#alice\n; hunter2\n</auth-user-pass>\n"_s
                                        << u"<auth-user-pass>\n#alice\n; hunter2\n</auth-user-pass>\n"_s;
    QTest::newRow("nested opaque payload inside a connection scope")
        << u"<connection>\n# gone\n<ca>\n# payload\n</ca>\n</connection>\n"_s << u"<connection>\n<ca>\n# payload\n</ca>\n</connection>\n"_s;
    // A comment cannot close a block: openvpn3 matches a closing tag against
    // the raw line, so this block is unterminated and is kept as it stands.
    QTest::newRow("a comment cannot close a block") << u"<ca>\nPEM\n</ca> # done\n"_s << u"<ca>\nPEM\n</ca> # done\n"_s;
    QTest::newRow("a hash in an unterminated block") << u"<ca>\nPEM # kept\n"_s << u"<ca>\nPEM # kept\n"_s;
    // A <connection> is a scope, but it is still a block: openvpn3 matches
    // its closing tag against the raw line too, so this one is unterminated
    // and everything in it is kept exactly as it stands rather than repaired.
    QTest::newRow("a comment cannot close a connection scope")
        << u"<connection>\nremote a.example.net # kept\n</connection> # done\n"_s
        << u"<connection>\nremote a.example.net # kept\n</connection> # done\n"_s;
    // The one place the two lexers disagree about quoting. openvpn3 lets the
    // backslash escape the apostrophe and stays inside the single quote, so
    // the hash is part of the value; OpenVPN 2 does not escape inside single
    // quotes, so for it the quote ends there and a comment follows. Cutting
    // would destroy the value openvpn3 -- the one that reads the stored
    // profile -- sees.
    QTest::newRow("escaped apostrophe inside single quotes") << u"setenv a 'x\\' # literal'\n"_s << u"setenv a 'x\\' # literal'\n"_s;
    QTest::newRow("escaped apostrophe, semicolon") << u"setenv a 'x\\' ; literal'\n"_s << u"setenv a 'x\\' ; literal'\n"_s;
    QTest::newRow("escaped apostrophe inside a connection scope")
        << u"<connection>\nsetenv a 'x\\' # literal'\n</connection>\n"_s << u"<connection>\nsetenv a 'x\\' # literal'\n</connection>\n"_s;

    // -- what goes, and exactly how much of it --
    //
    // Escaped whitespace is part of the value in front of it for both
    // lexers; only the unescaped whitespace after it separates the comment,
    // so only that goes with it.
    QTest::newRow("escaped trailing space") << u"setenv a value\\  # note\n"_s << u"setenv a value\\ \n"_s;
    QTest::newRow("escaped trailing tab") << u"setenv a value\\\t\t; note\n"_s << u"setenv a value\\\t\n"_s;
    QTest::newRow("escaped trailing space inside a connection scope")
        << u"<connection>\nsetenv a value\\  # note\n</connection>\n"_s << u"<connection>\nsetenv a value\\ \n</connection>\n"_s;
    // Two backslashes are one literal backslash, so the whitespace after
    // them is a separator again and goes with the comment.
    QTest::newRow("escaped backslash before a comment") << u"setenv a value\\\\ # note\n"_s << u"setenv a value\\\\\n"_s;

    // -- whitespace is ASCII whitespace --
    //
    // A directive line separates its words with ASCII whitespace, which is
    // all g_ascii_isspace() in the backend -- and all openvpn3's own lexer --
    // counts. U+00A0 and the other Unicode separators are literal bytes of
    // the value in front of them, so the separator run a comment is cut with
    // stops at one, and one cannot start the word a comment has to begin.
    QTest::newRow("unicode whitespace stays in a value") << u"setenv label value  # note\n"_s << u"setenv label value \n"_s;
    QTest::newRow("unicode whitespace alone before a comment") << u"setenv label value # note\n"_s << u"setenv label value # note\n"_s;
    QTest::newRow("unicode line separator stays in a value") << u"setenv label value  ; note\n"_s << u"setenv label value \n"_s;
    QTest::newRow("a value that is only unicode whitespace") << u"setenv label   # note\n"_s << u"setenv label  \n"_s;

    // -- a comment cannot be cut into a closing tag --
    //
    // openvpn3 matches every closing tag against the raw line, so a line
    // that is only a closing tag once its comment is off is not a boundary
    // for it. Cutting it here would invent one -- and move every directive
    // between it and the real closer out of the scope.
    QTest::newRow("a commented closer followed by a real one")
        << u"client\n<connection>\nremote a.example.net\n</connection> # not a closer\nremote b.example.net\n</connection>\n"_s
        << u"client\n<connection>\nremote a.example.net\n</connection> # not a closer\nremote b.example.net\n</connection>\n"_s;
    QTest::newRow("a commented closer at the top level")
        << u"client\n</connection> ; not a closer\nremote a.example.net\n"_s << u"client\n</connection> ; not a closer\nremote a.example.net\n"_s;
    QTest::newRow("a commented opaque closer followed by a real one")
        << u"<ca>\nPEM\n</ca> # not a closer\nMORE\n</ca>\n"_s << u"<ca>\nPEM\n</ca> # not a closer\nMORE\n</ca>\n"_s;
}

void Openvpn3ProfileTest::commentsAreDropped()
{
    QFETCH(QString, text);
    QFETCH(QString, expected);

    QCOMPARE(Openvpn3Profile::fromText(text).toText(), expected);
    // Dropping comments is idempotent: what came back has nothing left to
    // drop, so storing it and loading it again changes nothing more.
    QCOMPARE(Openvpn3Profile::fromText(expected).toText(), expected);
}

void Openvpn3ProfileTest::blankLinesAreDropped_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");

    // -- what goes --
    QTest::newRow("empty line") << u"client\n\nremote a.example.net\n"_s << u"client\nremote a.example.net\n"_s;
    QTest::newRow("run of empty lines") << u"client\n\n\n\nremote a.example.net\n"_s << u"client\nremote a.example.net\n"_s;
    QTest::newRow("leading and trailing") << u"\n\nclient\n\n"_s << u"client\n"_s;
    QTest::newRow("spaces") << u"client\n   \nremote a.example.net\n"_s << u"client\nremote a.example.net\n"_s;
    QTest::newRow("tabs") << u"client\n\t\t\nremote a.example.net\n"_s << u"client\nremote a.example.net\n"_s;
    QTest::newRow("form feed and spaces") << u"client\n \f \nremote a.example.net\n"_s << u"client\nremote a.example.net\n"_s;
    QTest::newRow("crlf") << u"client\r\n\r\nremote a.example.net\r\n"_s << u"client\r\nremote a.example.net\r\n"_s;
    QTest::newRow("crlf, spaces") << u"client\r\n  \r\nremote a.example.net\r\n"_s << u"client\r\nremote a.example.net\r\n"_s;
    QTest::newRow("a blank line after the last directive") << u"client\n\n"_s << u"client\n"_s;
    // The terminator a nonempty directive needs is part of that directive and
    // was never a line of its own, with or without a blank one after it.
    QTest::newRow("a terminator is not a blank line") << u"client\n"_s << u"client\n"_s;
    QTest::newRow("blank next to a comment") << u"\n# gone\n\nclient\n"_s << u"client\n"_s;
    QTest::newRow("inside a connection scope") << u"<connection>\n\nremote a.example.net\n  \n</connection>\n"_s
                                               << u"<connection>\nremote a.example.net\n</connection>\n"_s;
    QTest::newRow("a connection scope of nothing but blank lines") << u"<connection>\n\n\n</connection>\n"_s << u"<connection>\n</connection>\n"_s;

    // -- what stays --
    //
    // An opaque payload is content, not formatting: a blank line in a
    // certificate or a key is a byte of the data, and so is an empty
    // credential.
    QTest::newRow("opaque payload") << u"<ca>\n\nPEM\n\n</ca>\n"_s << u"<ca>\n\nPEM\n\n</ca>\n"_s;
    QTest::newRow("whitespace in an opaque payload") << u"<ca>\n   \nPEM\n</ca>\n"_s << u"<ca>\n   \nPEM\n</ca>\n"_s;
    QTest::newRow("an empty password") << u"<auth-user-pass>\nalice\n\n</auth-user-pass>\n"_s << u"<auth-user-pass>\nalice\n\n</auth-user-pass>\n"_s;
    QTest::newRow("an unknown payload") << u"<some-future-payload>\n\nkept\n\n</some-future-payload>\n"_s
                                        << u"<some-future-payload>\n\nkept\n\n</some-future-payload>\n"_s;
    QTest::newRow("a payload nested in a connection scope")
        << u"<connection>\n\n<ca>\n\nPEM\n</ca>\n\n</connection>\n"_s << u"<connection>\n<ca>\n\nPEM\n</ca>\n</connection>\n"_s;
    QTest::newRow("an unterminated block keeps its blank lines") << u"<ca>\n\nPEM\n\n"_s << u"<ca>\n\nPEM\n\n"_s;
    // The whitespace a directive line is padded with is part of that line.
    QTest::newRow("indentation is not a blank line") << u"   client   \n"_s << u"   client   \n"_s;
    // A value made of Unicode separators is a value: they separate nothing for
    // either lexer, so a line of them is a directive and not formatting. And
    // '\v' is not whitespace for g_ascii_isspace(), which is what the
    // backend's importer strips every line with, so it is not formatting here
    // either -- the two have to agree on which lines a profile has.
    QTest::newRow("a line of unicode whitespace") << u"client\n \nremote a.example.net\n"_s << u"client\n \nremote a.example.net\n"_s;
    QTest::newRow("a line of vertical tabs") << u"client\n\v\nremote a.example.net\n"_s << u"client\n\v\nremote a.example.net\n"_s;
}

void Openvpn3ProfileTest::blankLinesAreDropped()
{
    QFETCH(QString, text);
    QFETCH(QString, expected);

    QCOMPARE(Openvpn3Profile::fromText(text).toText(), expected);
    // Dropping them is idempotent: what came back has nothing left to drop,
    // so storing it and loading it again changes nothing more.
    QCOMPARE(Openvpn3Profile::fromText(expected).toText(), expected);
    // And no number of further passes does either.
    QString text2 = expected;
    for (int pass = 0; pass < 3; ++pass) {
        text2 = Openvpn3Profile::fromText(text2).toText();
    }
    QCOMPARE(text2, expected);
}

void Openvpn3ProfileTest::noMutatorCanPutABlankLineBack()
{
    // The document has no blank lines, so nothing done to it may write one.
    // Reloading what it serializes is how that is checked: a blank line a
    // mutator wrote would be dropped on the way back in, so the text would
    // not survive the round trip -- which is exactly the row a user edited
    // and then lost on the next load.
    Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);
    // Everything the editor does to a document, in one pass over it.
    profile.setDirective(u"cipher"_s, {u"AES-256-GCM"_s});
    profile.setPresent(u"pull"_s, true);
    profile.setArguments(profile.indexOf(u"dev"_s), {u"tap"_s});
    profile.setArguments(profile.indexOf(u"key-direction"_s), {});
    profile.setBlock(u"connection"_s, u"\nremote b.example.net\n\n"_s);
    profile.setBody(profile.indexOf(u"ca"_s), u"\nNEW-CA\n"_s);
    profile.append(Openvpn3Profile::directive(u"verb"_s));
    profile.insert(0, Openvpn3Profile::directive(u"client"_s));
    profile.move(0, 1);
    profile.removeAt(0);
    profile.removeAll(u"proto"_s);

    const QString text = profile.toText();
    // A blank line typed into a scope went the way one read from a file does;
    // the one typed into the opaque payload is content and stayed.
    QVERIFY(text.contains(u"<connection>\nremote b.example.net\n</connection>\n"_s));
    QVERIFY(text.contains(u"<ca>\n\nNEW-CA\n</ca>\n"_s));
    // Nothing between two directives is a blank line.
    QVERIFY(!text.contains(u"\n\nclient"_s));
    QVERIFY(!text.contains(u"\n\nverb"_s));
    // And the document is a fixed point: a reload changes nothing. A blank
    // line any of the above wrote would be dropped on the way back in, so the
    // text would not come back the same.
    QCOMPARE(Openvpn3Profile::fromText(text).toText(), text);
    QCOMPARE(Openvpn3Profile::fromText(Openvpn3Profile::fromText(text).toText()).toText(), text);
}

void Openvpn3ProfileTest::aMalformedCloserNeverBecomesAScopeBoundary()
{
    // The reason the line above is kept rather than repaired: the scope runs
    // to the closing tag openvpn3 sees, and serializing has to leave it
    // exactly there.
    const QString text = u"client\n<connection>\nremote a.example.net\n</connection> # not a closer\nremote b.example.net\n</connection>\n"_s;
    const Openvpn3Profile profile = Openvpn3Profile::fromText(text);

    QCOMPARE(profile.count(), 2);
    QCOMPARE(profile.at(1).name, u"connection"_s);
    QVERIFY(profile.blockBody(u"connection"_s).contains(u"remote b.example.net"_s));
    QCOMPARE(profile.remoteHosts(), QStringList({u"a.example.net"_s, u"b.example.net"_s}));

    // And the document it hands back parses to the same thing, scope and all.
    const Openvpn3Profile again = Openvpn3Profile::fromText(profile.toText());
    QCOMPARE(again.toText(), text);
    QCOMPARE(again.count(), 2);
    QCOMPARE(again.blockBody(u"connection"_s), profile.blockBody(u"connection"_s));
    QCOMPARE(again.remoteHosts(), profile.remoteHosts());
}

void Openvpn3ProfileTest::noEntryIsEverFormatting()
{
    // There is no comment entry and no blank entry to be had, so no table
    // built from a profile can have a row for either -- whatever the document
    // it was loaded from. Directive and Block are the only kinds there are.
    for (const QString &text : {kRich, u"# one\n; two\n"_s, u"client # trailing"_s, u"\nclient\n\n"_s}) {
        const Openvpn3Profile profile = Openvpn3Profile::fromText(text);
        for (const Openvpn3Entry &entry : profile.entries()) {
            QVERIFY(entry.isDirective() || entry.isBlock());
            QVERIFY(!profile.sourceAt(profile.indexOfId(entry.id())).trimmed().isEmpty());
        }
    }
    // Nothing is left of a document that was only ever formatting.
    QVERIFY(Openvpn3Profile::fromText(u"# one\n; two\n"_s).isEmpty());
    QVERIFY(Openvpn3Profile::fromText(u"\n\n   \n"_s).isEmpty());
    QCOMPARE(Openvpn3Profile::fromText(u"client # trailing"_s).toText(), u"client"_s);
}

void Openvpn3ProfileTest::aScopeBodyNeverTakesAComment()
{
    // A <connection> body is the scope's directives, and the editor hands it
    // over as text. A comment put in that way has to go the same way one
    // read from a file does: otherwise it would be saved and then vanish the
    // next time the connection was loaded.
    Openvpn3Profile profile = Openvpn3Profile::fromText(u"client\n<connection>\nremote a.example.net\n</connection>\n"_s);
    QCOMPARE(profile.at(1).name, u"connection"_s);

    profile.setBody(1, u"# mine\nremote b.example.net # here\n"_s);
    QCOMPARE(profile.toText(), u"client\n<connection>\nremote b.example.net\n</connection>\n"_s);

    // So does one in a scope built from scratch, or set by name.
    QCOMPARE(Openvpn3Profile::block(u"connection"_s, u"remote c.example.net # here\n"_s).body, u"remote c.example.net\n"_s);
    profile.setBlock(u"connection"_s, u"remote d.example.net ; here\n"_s);
    QCOMPARE(profile.toText(), u"client\n<connection>\nremote d.example.net\n</connection>\n"_s);

    // An opaque payload is content rather than directives, so its body is
    // taken exactly as it is given -- key material included.
    profile.setBlock(u"ca"_s, u"# payload\nA;B#C\n"_s);
    QVERIFY(profile.toText().contains(u"<ca>\n# payload\nA;B#C\n</ca>\n"_s));
    QCOMPARE(Openvpn3Profile::block(u"key"_s, u"KEY # payload\n"_s).body, u"KEY # payload\n"_s);
}

void Openvpn3ProfileTest::aScopeBodyNeverTakesABlankLine()
{
    // The same for the blank lines a user spaces a <connection> body out with:
    // the scope holds directives, and an entry that is not there after the
    // next load is not an entry to keep now.
    Openvpn3Profile profile = Openvpn3Profile::fromText(u"client\n<connection>\nremote a.example.net\n</connection>\n"_s);

    profile.setBody(1, u"\nremote b.example.net\n   \nhttp-proxy proxy.example.net 8080\n\n"_s);
    QCOMPARE(profile.toText(), u"client\n<connection>\nremote b.example.net\nhttp-proxy proxy.example.net 8080\n</connection>\n"_s);

    // So does one in a scope built from scratch, or set by name.
    QCOMPARE(Openvpn3Profile::block(u"connection"_s, u"\nremote c.example.net\n\n"_s).body, u"remote c.example.net\n"_s);
    profile.setBlock(u"connection"_s, u"\n\nremote d.example.net\n"_s);
    QCOMPARE(profile.toText(), u"client\n<connection>\nremote d.example.net\n</connection>\n"_s);

    // An opaque payload is content, so every line of what is typed into it
    // stays -- a blank one at either end of a key included.
    profile.setBlock(u"key"_s, u"\nKEY\n\n"_s);
    QVERIFY(profile.toText().contains(u"<key>\n\nKEY\n\n</key>\n"_s));
    QCOMPARE(Openvpn3Profile::block(u"ca"_s, u"\nPEM\n"_s).body, u"\nPEM\n"_s);
}

void Openvpn3ProfileTest::parsesEntryKinds()
{
    const Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);

    QCOMPARE(profile.at(0).kind, Openvpn3Entry::Directive);
    QCOMPARE(profile.at(0).name, QStringLiteral("client"));
    QVERIFY(profile.at(0).arguments.isEmpty());
    // The blank line between the remotes and this is not an entry, so the
    // directive below it follows the last remote straight away.
    QCOMPARE(profile.at(6).kind, Openvpn3Entry::Directive);
    QCOMPARE(profile.at(6).name, QStringLiteral("auth-user-pass"));
    QCOMPARE(profile.value(QStringLiteral("dev")), QStringLiteral("tun"));
    // A hash inside quotes is part of the value, not the start of a comment.
    QCOMPARE(profile.arguments(QStringLiteral("setenv")), QStringList({QStringLiteral("opt"), QStringLiteral("single quoted value")}));
    QVERIFY(profile.contains(QStringLiteral("auth-user-pass")));
    QVERIFY(!profile.contains(QStringLiteral("tls-crypt")));
    QCOMPARE(profile.arguments(QStringLiteral("some-directive-we-have-never-heard-of")),
             QStringList({QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}));
}

void Openvpn3ProfileTest::keepsDuplicatesInOrder()
{
    const Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);
    const QList<int> remotes = profile.indexesOf(QStringLiteral("remote"));

    // Three top-level remotes, two of them identical, in document order.
    QCOMPARE(remotes.size(), 3);
    QCOMPARE(profile.at(remotes.at(0)).value(), QStringLiteral("vpn1.example.net"));
    QCOMPARE(profile.at(remotes.at(1)).value(), QStringLiteral("vpn2.example.net"));
    QCOMPARE(profile.at(remotes.at(2)).value(), QStringLiteral("vpn1.example.net"));
    QCOMPARE(profile.at(remotes.at(1)).arguments,
             QStringList({QStringLiteral("vpn2.example.net"), QStringLiteral("443"), QStringLiteral("tcp")}));
}

void Openvpn3ProfileTest::keepsBlocksVerbatim()
{
    const Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);

    // Including the hash in there and the blank line below it, which are
    // payload and not formatting.
    QCOMPARE(profile.blockBody(QStringLiteral("ca")),
             QStringLiteral("-----BEGIN CERTIFICATE-----\nMIIBsyntheticTESTDATA # payload, not a comment\n\n-----END CERTIFICATE-----\n"));
    const int index = profile.indexOf(QStringLiteral("ca"));
    QVERIFY(index >= 0);
    QVERIFY(profile.at(index).isBlock());
}

void Openvpn3ProfileTest::doesNotDescendIntoConnectionBlocks()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);

    // The remote inside <connection> is part of that block's body, so it is
    // neither listed nor touched when the top-level remotes are edited.
    QCOMPARE(profile.indexesOf(QStringLiteral("remote")).size(), 3);
    QVERIFY(profile.blockBody(QStringLiteral("connection")).contains(QStringLiteral("fallback.example.net")));

    profile.setArguments(profile.indexOf(QStringLiteral("remote")), {QStringLiteral("new.example.net")});
    QVERIFY(profile.toText().contains(QStringLiteral("remote fallback.example.net 1194 udp")));
    QVERIFY(profile.toText().contains(QStringLiteral("http-proxy proxy.example.net 8080")));
}

void Openvpn3ProfileTest::parsesQuoting_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<QStringList>("words");

    QTest::newRow("plain") << QStringLiteral("remote a.example.net 1194") << QStringList({u"remote"_s, u"a.example.net"_s, u"1194"_s});
    QTest::newRow("double quotes") << QStringLiteral("setenv x \"a b\"") << QStringList({u"setenv"_s, u"x"_s, u"a b"_s});
    QTest::newRow("single quotes") << QStringLiteral("setenv x 'a b'") << QStringList({u"setenv"_s, u"x"_s, u"a b"_s});
    QTest::newRow("escape in double") << QStringLiteral("setenv x \"a\\\"b\"") << QStringList({u"setenv"_s, u"x"_s, u"a\"b"_s});
    // Deliberate, and the one place where following OpenVPN 2 rather than
    // openvpn3 is visible: openvpn3 would read 'a\b' as "ab" and would stay
    // inside the quote past an escaped apostrophe. The line is never cut, so
    // what it means to openvpn3 is intact until somebody edits the entry.
    // See docs/limitations.md.
    QTest::newRow("no escape in single") << QStringLiteral("setenv x 'a\\b'") << QStringList({u"setenv"_s, u"x"_s, u"a\\b"_s});
    QTest::newRow("escaped apostrophe closes the quote for OpenVPN 2")
        << QStringLiteral("setenv x 'a\\' tail") << QStringList({u"setenv"_s, u"x"_s, u"a\\"_s, u"tail"_s});
    // Outside quotes a backslash escapes the next character for both lexers:
    // OpenVPN 2's parse_line() leaves its escape handling off only inside
    // single quotes, and openvpn3's StandardLex never turns it off at all. An
    // escaped separator is therefore part of the value in front of it, not the
    // end of it.
    QTest::newRow("escaped space outside quotes")
        << QStringLiteral("setenv x hello\\ world") << QStringList({u"setenv"_s, u"x"_s, u"hello world"_s});
    QTest::newRow("escaped tab outside quotes")
        << QStringLiteral("setenv x hello\\\tworld") << QStringList({u"setenv"_s, u"x"_s, u"hello\tworld"_s});
    QTest::newRow("escaped hash outside quotes")
        << QStringLiteral("setenv x \\#1") << QStringList({u"setenv"_s, u"x"_s, u"#1"_s});
    QTest::newRow("escaped quote outside quotes")
        << QStringLiteral("setenv x a\\\"b") << QStringList({u"setenv"_s, u"x"_s, u"a\"b"_s});
    QTest::newRow("escaped backslash outside quotes")
        << QStringLiteral("setenv x a\\\\b") << QStringList({u"setenv"_s, u"x"_s, u"a\\b"_s});
    // Nothing to escape at the end of a line, so it stays the character it is.
    QTest::newRow("lone trailing backslash")
        << QStringLiteral("setenv x a\\") << QStringList({u"setenv"_s, u"x"_s, u"a\\"_s});
    QTest::newRow("trailing comment") << QStringLiteral("dev tun # a comment") << QStringList({u"dev"_s, u"tun"_s});
    QTest::newRow("empty argument") << QStringLiteral("setenv x \"\"") << QStringList({u"setenv"_s, u"x"_s, QString()});
    QTest::newRow("comma inside quotes") << QStringLiteral("verify-x509-name \"C=NO, O=X\" subject")
                                         << QStringList({u"verify-x509-name"_s, u"C=NO, O=X"_s, u"subject"_s});
}

void Openvpn3ProfileTest::parsesQuoting()
{
    QFETCH(QString, line);
    QFETCH(QStringList, words);
    QCOMPARE(Openvpn3Profile::splitArguments(line), words);
}

void Openvpn3ProfileTest::anEscapedSeparatorIsPartOfItsValue()
{
    // Reading the line as three arguments would be bad enough on its own, but
    // the damage lands on save: editing the label would rewrite the escaped
    // space as a separator, and the one value would become two.
    Openvpn3Profile profile = Openvpn3Profile::fromText(u"setenv label hello\\ world\n"_s);
    const int index = profile.indexOf(u"setenv"_s);
    QVERIFY(index >= 0);
    QCOMPARE(profile.at(index).arguments, QStringList({u"label"_s, u"hello world"_s}));

    profile.setArguments(index, {u"tag"_s, profile.at(index).arguments.value(1)});
    // Rewritten, so quoted rather than escaped -- but the same one value.
    QCOMPARE(Openvpn3Profile::fromText(profile.toText()).arguments(u"setenv"_s),
             QStringList({u"tag"_s, u"hello world"_s}));
}

void Openvpn3ProfileTest::anEscapedTrailingSeparatorIsPartOfItsValue_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("value");

    // The escaped space is the last character of the value, so there is
    // nothing after it to tell the lexer where the value ended. Trimming the
    // line before splitting it cannot tell this from padding and loses it.
    QTest::newRow("escaped space at end of line") << u"setenv label value\\ \n"_s << u"value "_s;
    QTest::newRow("escaped tab at end of line") << u"setenv label value\\\t\n"_s << u"value\t"_s;
    QTest::newRow("escaped space before a comment") << u"setenv label value\\  # note\n"_s << u"value "_s;
    QTest::newRow("escaped space with CRLF") << u"setenv label value\\ \r\n"_s << u"value "_s;
    QTest::newRow("escaped space and no final newline") << u"setenv label value\\ "_s << u"value "_s;
    // An even number of backslashes are escaped backslashes, so the space
    // after them separates after all and the value ends in one backslash.
    QTest::newRow("escaped backslash then a separator") << u"setenv label value\\\\ \n"_s << u"value\\"_s;
    // Padding is padding, however much of it there is.
    QTest::newRow("unescaped trailing space") << u"setenv label value   \n"_s << u"value"_s;
}

void Openvpn3ProfileTest::anEscapedTrailingSeparatorIsPartOfItsValue()
{
    QFETCH(QString, line);
    QFETCH(QString, value);

    Openvpn3Profile profile = Openvpn3Profile::fromText(line);
    const int index = profile.indexOf(u"setenv"_s);
    QVERIFY(index >= 0);
    QCOMPARE(profile.at(index).arguments, QStringList({u"label"_s, value}));

    // And the damage this does when it is read wrongly: editing the label
    // rewrites the line, so a value read short is a value saved short.
    profile.setArguments(index, {u"tag"_s, profile.at(index).arguments.value(1)});
    QCOMPARE(Openvpn3Profile::fromText(profile.toText()).arguments(u"setenv"_s), QStringList({u"tag"_s, value}));
}

void Openvpn3ProfileTest::untouchedEntriesKeepTheirQuoting()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);
    const int index = profile.indexOf(QStringLiteral("setenv"));

    // Assigning the same values is not a change, so the single quotes stay.
    profile.setArguments(index, profile.at(index).arguments);
    QVERIFY(profile.toText().contains(QStringLiteral("setenv opt 'single quoted value'")));
}

void Openvpn3ProfileTest::editingOneEntryLeavesTheOthersAlone()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);
    const QList<int> remotes = profile.indexesOf(QStringLiteral("remote"));

    profile.setArguments(remotes.at(1), {QStringLiteral("vpn3.example.net"), QStringLiteral("443"), QStringLiteral("tcp")});
    QString text = profile.toText();

    QCOMPARE(text.count(QStringLiteral("remote vpn1.example.net 1194 udp\n")), 2);
    QVERIFY(text.contains(QStringLiteral("remote vpn3.example.net 443 tcp\n")));
    QVERIFY(!text.contains(QStringLiteral("vpn2.example.net")));
    // Everything else, the unknown directive included, is still byte for byte
    // what the document was once its formatting was gone.
    QCOMPARE(text.replace(QStringLiteral("remote vpn3.example.net 443 tcp"), QStringLiteral("remote vpn2.example.net 443 tcp")), kRichKept);
}

void Openvpn3ProfileTest::editingKeepsArgumentsItWasNotToldAbout()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(QStringLiteral("client\nremote old.example.net 1194 udp\n"));
    const int index = profile.indexOf(QStringLiteral("remote"));

    // Changing only the host: the port and the transport are still there.
    QStringList arguments = profile.at(index).arguments;
    arguments[0] = QStringLiteral("new.example.net");
    profile.setArguments(index, arguments);

    QCOMPARE(profile.toText(), QStringLiteral("client\nremote new.example.net 1194 udp\n"));
}

void Openvpn3ProfileTest::setDirectiveAppendsWhenMissing()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(QStringLiteral("client\nremote a.example.net\n"));

    profile.setDirective(QStringLiteral("cipher"), {QStringLiteral("AES-256-GCM")});
    QCOMPARE(profile.toText(), QStringLiteral("client\nremote a.example.net\ncipher AES-256-GCM\n"));

    profile.setDirective(QStringLiteral("cipher"), {QStringLiteral("AES-128-GCM")});
    QCOMPARE(profile.toText(), QStringLiteral("client\nremote a.example.net\ncipher AES-128-GCM\n"));
}

void Openvpn3ProfileTest::setPresentKeepsExistingArguments()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(QStringLiteral("client\nauth-user-pass creds.txt\n"));

    profile.setPresent(QStringLiteral("auth-user-pass"), true);
    QCOMPARE(profile.toText(), QStringLiteral("client\nauth-user-pass creds.txt\n"));

    profile.setPresent(QStringLiteral("auth-user-pass"), false);
    QCOMPARE(profile.toText(), QStringLiteral("client\n"));

    profile.setPresent(QStringLiteral("auth-user-pass"), true);
    QCOMPARE(profile.toText(), QStringLiteral("client\nauth-user-pass\n"));
}

void Openvpn3ProfileTest::removeAllRemovesEveryDuplicate()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);

    profile.removeAll(QStringLiteral("remote"));
    QVERIFY(profile.indexesOf(QStringLiteral("remote")).isEmpty());
    // The one inside <connection> is untouched.
    QVERIFY(profile.toText().contains(QStringLiteral("remote fallback.example.net")));
}

void Openvpn3ProfileTest::blocksCanBeReplacedAndAdded()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);

    profile.setBlock(QStringLiteral("ca"), QStringLiteral("NEW-CA\n"));
    QCOMPARE(profile.blockBody(QStringLiteral("ca")), QStringLiteral("NEW-CA\n"));
    QVERIFY(profile.toText().contains(QStringLiteral("<ca>\nNEW-CA\n</ca>\n")));
    QVERIFY(!profile.toText().contains(QStringLiteral("MIIBsyntheticTESTDATA")));

    profile.setBlock(QStringLiteral("tls-crypt"), QStringLiteral("KEY"));
    // A body without its own newline still gets a well-formed closing tag.
    QVERIFY(profile.toText().endsWith(QStringLiteral("<tls-crypt>\nKEY\n</tls-crypt>\n")));
}

void Openvpn3ProfileTest::entriesCanBeInsertedMovedAndRemoved()
{
    Openvpn3Profile profile = Openvpn3Profile::fromText(QStringLiteral("client\nremote a.example.net\nremote b.example.net\n"));

    const QList<int> remotes = profile.indexesOf(QStringLiteral("remote"));
    profile.move(remotes.at(1), remotes.at(0));
    QCOMPARE(profile.toText(), QStringLiteral("client\nremote b.example.net\nremote a.example.net\n"));

    profile.insert(0, Openvpn3Profile::directive(QStringLiteral("pull")));
    QVERIFY(profile.toText().startsWith(QStringLiteral("pull\nclient\n")));

    profile.append(Openvpn3Profile::directive(QStringLiteral("remote"), {QStringLiteral("c.example.net")}));
    QCOMPARE(profile.indexesOf(QStringLiteral("remote")).size(), 3);

    profile.removeAt(profile.indexesOf(QStringLiteral("remote")).at(1));
    QCOMPARE(profile.indexesOf(QStringLiteral("remote")).size(), 2);
    QVERIFY(!profile.toText().contains(QStringLiteral("a.example.net")));
}

void Openvpn3ProfileTest::quotingRoundTrips_data()
{
    QTest::addColumn<QString>("argument");

    QTest::newRow("plain") << QStringLiteral("value");
    QTest::newRow("spaces") << QStringLiteral("a value with spaces");
    QTest::newRow("double quote") << QStringLiteral("a \"quoted\" value");
    QTest::newRow("single quote") << QStringLiteral("it's here");
    QTest::newRow("backslash") << QStringLiteral("C:\\path\\to");
    QTest::newRow("hash") << QStringLiteral("not#a#comment");
    QTest::newRow("empty") << QString();
}

void Openvpn3ProfileTest::quotingRoundTrips()
{
    QFETCH(QString, argument);

    const QString line = QStringLiteral("setenv x ") + Openvpn3Profile::quoteArgument(argument);
    QCOMPARE(Openvpn3Profile::splitArguments(line).value(2), argument);

    // And the same through a whole document.
    Openvpn3Profile profile;
    profile.append(Openvpn3Profile::directive(QStringLiteral("setenv"), {QStringLiteral("x"), argument}));
    QCOMPARE(Openvpn3Profile::fromText(profile.toText()).arguments(QStringLiteral("setenv")).value(1), argument);
}

void Openvpn3ProfileTest::editingKeepsAUnicodeWhitespaceValue()
{
    // U+00A0 separates nothing, so it is a literal byte of the value it sits
    // in -- but QString::trimmed(), which every line goes through before its
    // arguments are read, takes it off the end of one all the same. An
    // argument that ends in Unicode whitespace therefore has to be written
    // back quoted, or editing the directive next to it truncates it.
    const QString value = u"value "_s;
    Openvpn3Profile profile = Openvpn3Profile::fromText(u"setenv label \"value \"\n"_s);
    QCOMPARE(profile.arguments(u"setenv"_s), QStringList({u"label"_s, value}));

    // Rename the first argument; the second goes back as it was read.
    profile.setArguments(0, {u"label2"_s, value});
    const Openvpn3Profile again = Openvpn3Profile::fromText(profile.toText());
    QCOMPARE(again.arguments(u"setenv"_s), QStringList({u"label2"_s, value}));

    // And the document that came back serializes the same value once more.
    QCOMPARE(Openvpn3Profile::fromText(again.toText()).arguments(u"setenv"_s), QStringList({u"label2"_s, value}));
}

void Openvpn3ProfileTest::unterminatedBlockIsKeptVerbatim()
{
    const auto text = QStringLiteral("client\n<ca>\nPEM-LINE\nanother\n");
    const Openvpn3Profile profile = Openvpn3Profile::fromText(text);

    QCOMPARE(profile.count(), 2);
    QCOMPARE(profile.toText(), text);
}

void Openvpn3ProfileTest::spotsKeyMaterialThatCouldNeedAPassphrase_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("expected");

    QTest::newRow("nothing") << u"client\nremote a.example.org\n"_s << false;
    QTest::newRow("certificate only") << u"client\n<ca>\nPEM\n</ca>\n"_s << false;
    QTest::newRow("plain key") << u"client\n<key>\n-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n</key>\n"_s << false;
    QTest::newRow("pkcs8 encrypted key")
        << u"client\n<key>\n-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n-----END ENCRYPTED PRIVATE KEY-----\n</key>\n"_s << true;
    QTest::newRow("traditional encrypted key") << u"client\n<key>\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-256-CBC,00\nAAAA\n</key>\n"_s << true;
    QTest::newRow("pkcs12 block") << u"client\n<pkcs12>\nAAAA\n</pkcs12>\n"_s << true;
    // A file reference cannot be inspected, so it has to be assumed.
    QTest::newRow("key file reference") << u"client\nkey /etc/openvpn/client.key\n"_s << true;
    QTest::newRow("pkcs12 file reference") << u"client\npkcs12 /etc/openvpn/client.p12\n"_s << true;
}

void Openvpn3ProfileTest::spotsKeyMaterialThatCouldNeedAPassphrase()
{
    QFETCH(QString, text);
    QFETCH(bool, expected);

    QCOMPARE(Openvpn3Profile::fromText(text).mayNeedPrivateKeyPassphrase(), expected);
}

// -- what openvpn3 sees, as opposed to what the top level of the document says --

void Openvpn3ProfileTest::optionsAreFoundInsideConnectionBlocks()
{
    const Openvpn3Profile profile = Openvpn3Profile::fromText(
        u"client\n<connection>\nremote fallback.example.net 1194 udp\nauth-user-pass\n</connection>\n"_s);

    // The top level is what the editor edits, and it has no auth-user-pass.
    QVERIFY(!profile.contains(u"auth-user-pass"_s));
    QVERIFY(profile.indexesOf(u"remote"_s).isEmpty());

    // What the connection actually needs is another question, and the one
    // that decides whether a username and password are stored for it.
    QVERIFY(profile.containsOption(u"auth-user-pass"_s));
    QCOMPARE(profile.optionsNamed(u"remote"_s).size(), 1);
    QCOMPARE(profile.optionsNamed(u"remote"_s).constFirst().value(), u"fallback.example.net"_s);
}

void Openvpn3ProfileTest::opaqueBlocksAreNeverDescendedInto()
{
    // A certificate whose base64 happens to begin with a word the editor
    // knows is payload, not a directive.
    const Openvpn3Profile profile = Openvpn3Profile::fromText(
        u"client\n<connection>\n<ca>\nremote this.is.payload\nauth-user-pass\n</ca>\n</connection>\n"_s);

    QVERIFY(!profile.containsOption(u"remote"_s));
    QVERIFY(!profile.containsOption(u"auth-user-pass"_s));
    // The block itself is one entry, carried across as it stands.
    QCOMPARE(profile.optionsNamed(u"ca"_s).size(), 1);
    QVERIFY(profile.optionsNamed(u"ca"_s).constFirst().isBlock());
    QVERIFY(profile.optionsNamed(u"ca"_s).constFirst().body.contains(u"this.is.payload"_s));
}

void Openvpn3ProfileTest::nestedInlineCredentialsAreFoundAsABlock()
{
    const Openvpn3Profile profile =
        Openvpn3Profile::fromText(u"client\nremote a.example.net\n<connection>\n<auth-user-pass>\nalice\npw\n</auth-user-pass>\n</connection>\n"_s);

    const QList<Openvpn3Entry> found = profile.optionsNamed(u"auth-user-pass"_s);
    QCOMPARE(found.size(), 1);
    QVERIFY(found.constFirst().isBlock());
    QVERIFY(profile.needsNormalization());
}

void Openvpn3ProfileTest::remoteHostsComeFromEveryScope_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QStringList>("hosts");

    QTest::newRow("top level") << u"client\nremote a.example.net 1194\n"_s << QStringList{u"a.example.net"_s};
    QTest::newRow("inside a connection block")
        << u"client\n<connection>\nremote b.example.net 1194\n</connection>\n"_s << QStringList{u"b.example.net"_s};
    QTest::newRow("both") << u"client\nremote a.example.net\n<connection>\nremote b.example.net\n</connection>\n"_s
                          << QStringList{u"a.example.net"_s, u"b.example.net"_s};
    // A row the user added and never filled in is a remote with no host; it
    // is listed, because whether that is good enough is not this class's call.
    QTest::newRow("blank row") << u"client\nremote\n"_s << QStringList{QString()};
    QTest::newRow("none") << u"client\ndev tun\n"_s << QStringList{};
}

void Openvpn3ProfileTest::remoteHostsComeFromEveryScope()
{
    QFETCH(QString, text);
    QFETCH(QStringList, hosts);

    QCOMPARE(Openvpn3Profile::fromText(text).remoteHosts(), hosts);
}

void Openvpn3ProfileTest::normalizationIsNeededOnlyForFilesAndCredentials_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("expected");

    QTest::newRow("self-contained") << u"client\nremote a.example.net\n<ca>\nPEM\n</ca>\n"_s << false;
    QTest::newRow("unknown directives are not our business") << u"client\nremote a.example.net\nsomething /a/path\n"_s << false;
    QTest::newRow("ca file") << u"client\nca /etc/openvpn/ca.crt\n"_s << true;
    QTest::newRow("key file") << u"client\nkey client.key\n"_s << true;
    QTest::newRow("pkcs12 file") << u"client\npkcs12 client.p12\n"_s << true;
    QTest::newRow("tls-crypt file") << u"client\ntls-crypt tc.key\n"_s << true;
    QTest::newRow("credentials file") << u"client\nauth-user-pass creds.txt\n"_s << true;
    QTest::newRow("inline credentials") << u"client\n<auth-user-pass>\nalice\npw\n</auth-user-pass>\n"_s << true;
    QTest::newRow("bare auth-user-pass") << u"client\nauth-user-pass\n"_s << false;
    QTest::newRow("explicit inline marker") << u"client\nca [inline]\n<ca>\nPEM\n</ca>\n"_s << false;
    QTest::newRow("a crl directory is not inlined") << u"client\ncrl-verify /etc/openvpn/crls dir\n"_s << false;
    QTest::newRow("a file named inside a connection block") << u"client\n<connection>\nca ca.crt\n</connection>\n"_s << true;
}

void Openvpn3ProfileTest::normalizationIsNeededOnlyForFilesAndCredentials()
{
    QFETCH(QString, text);
    QFETCH(bool, expected);

    QCOMPARE(Openvpn3Profile::fromText(text).needsNormalization(), expected);
}

void Openvpn3ProfileTest::entriesKeepTheirIdentityWhileTheDocumentChanges()
{
    // Rows of the server table are remembered by entry, not by position, so
    // an entry has to stay the same entry while the document around it moves.
    Openvpn3Profile profile = Openvpn3Profile::fromText(u"client\nremote a.example.net\ndev tun\nremote b.example.net\n"_s);
    const QList<int> remotes = profile.indexesOf(u"remote"_s);
    const quint64 first = profile.at(remotes.at(0)).id();
    const quint64 second = profile.at(remotes.at(1)).id();
    QVERIFY(first != second);

    profile.setArguments(remotes.at(0), {u"c.example.net"_s});
    QCOMPARE(profile.at(remotes.at(0)).id(), first);

    profile.insert(0, Openvpn3Profile::directive(u"pull"_s));
    QCOMPARE(profile.indexOfId(first), 2);
    QCOMPARE(profile.indexOfId(second), 4);

    profile.removeAt(profile.indexOfId(first));
    QCOMPARE(profile.indexOfId(first), -1);
    QCOMPARE(profile.indexOfId(second), 3);
}

void Openvpn3ProfileTest::parsedEntriesHaveDistinctIdentities()
{
    const Openvpn3Profile profile = Openvpn3Profile::fromText(kRich);

    QSet<quint64> ids;
    for (const Openvpn3Entry &entry : profile.entries()) {
        QVERIFY(entry.id() != 0);
        ids.insert(entry.id());
    }
    QCOMPARE(ids.size(), profile.count());

    // Two documents never share one, either: a reload is a different document.
    const Openvpn3Profile other = Openvpn3Profile::fromText(kRich);
    for (const Openvpn3Entry &entry : other.entries()) {
        QVERIFY(!ids.contains(entry.id()));
    }
}

void Openvpn3ProfileTest::crlfIsVisibleAsTheDocumentsConvention_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("expected");

    QTest::newRow("unix") << u"client\nremote a.example.net\n"_s << false;
    QTest::newRow("dos") << u"client\r\nremote a.example.net\r\n"_s << true;
    QTest::newRow("dos with a block") << u"client\r\n<ca>\r\nPEM\r\n</ca>\r\n"_s << true;
    // One stray terminator does not make it a DOS file.
    QTest::newRow("mostly unix") << u"client\nremote a.example.net\r\ndev tun\n"_s << false;
    QTest::newRow("empty") << QString() << false;
}

void Openvpn3ProfileTest::crlfIsVisibleAsTheDocumentsConvention()
{
    QFETCH(QString, text);
    QFETCH(bool, expected);

    QCOMPARE(Openvpn3Profile::fromText(text).usesCrlf(), expected);
}

QTEST_GUILESS_MAIN(Openvpn3ProfileTest)

#include "openvpn3profiletest.moc"
