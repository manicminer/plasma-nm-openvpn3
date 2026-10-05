/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTest>

#include "openvpn3directiveswidget.h"
#include "openvpn3profile.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
enum Column {
    KindColumn,
    NameColumn,
    ArgumentsColumn,
};

/** A profile with everything the table has to carry: repeated directives, a
 * block, a directive nobody has heard of -- and the formatting, the comments
 * and the blank lines, that are not the table's to carry. */
const auto kProfile = QStringLiteral(
    "# a comment\n"
    "client\n"
    "\n"
    "remote a.example.org 1194 udp\n"
    "remote b.example.org 443 tcp # the fallback\n"
    "push-peer-info\n"
    "setenv opt 'single quoted value'\n"
    "\n"
    "<ca>\n"
    "-----BEGIN CERTIFICATE-----\n"
    "SYNTHETIC # payload, not a comment\n"
    "-----END CERTIFICATE-----\n"
    "</ca>\n"
    "some-directive-we-have-never-heard-of 1 2 3\n");

/** kProfile as the table holds it: without the formatting, with everything
 * else exactly where it was. */
const auto kProfileKept = QStringLiteral(
    "client\n"
    "remote a.example.org 1194 udp\n"
    "remote b.example.org 443 tcp\n"
    "push-peer-info\n"
    "setenv opt 'single quoted value'\n"
    "<ca>\n"
    "-----BEGIN CERTIFICATE-----\n"
    "SYNTHETIC # payload, not a comment\n"
    "-----END CERTIFICATE-----\n"
    "</ca>\n"
    "some-directive-we-have-never-heard-of 1 2 3\n");

QTableWidget *table(Openvpn3DirectivesWidget *widget)
{
    return widget->findChild<QTableWidget *>(u"openvpn3_directives_table"_s);
}

QPlainTextEdit *body(Openvpn3DirectivesWidget *widget)
{
    return widget->findChild<QPlainTextEdit *>(u"openvpn3_directives_body"_s);
}

QPushButton *button(Openvpn3DirectivesWidget *widget, const QString &name)
{
    return widget->findChild<QPushButton *>(name);
}

int rowOf(const QTableWidget *view, const QString &name, int nth = 0)
{
    int seen = 0;
    for (int row = 0; row < view->rowCount(); ++row) {
        if (view->item(row, NameColumn) && view->item(row, NameColumn)->text() == name && seen++ == nth) {
            return row;
        }
    }
    return -1;
}

QStringList namesIn(const QTableWidget *view)
{
    QStringList names;
    for (int row = 0; row < view->rowCount(); ++row) {
        names.append(view->item(row, NameColumn) ? view->item(row, NameColumn)->text() : QString());
    }
    return names;
}
}

/**
 * The generic entry table, driven the way a user drives it.
 *
 * Every assertion here goes through the widgets: a cell is edited by setting
 * its text, a row is removed by clicking Remove. What the profile looks like
 * afterwards is what the connection would be saved with.
 */
class Openvpn3DirectivesTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void everyLineOfTheProfileIsARow();
    void anUntouchedProfileComesBackUnchanged();
    void repeatedDirectivesAreNeitherMergedNorReordered();
    void editingOneDirectiveLeavesTheRestAlone();
    void editingADirectivesArgumentsRespectsQuoting();
    void renamingADirectiveKeepsItsArguments();
    void commentsAreNotRowsAtAll();
    void blankLinesAreNotRowsAtAll();
    void aCommentTypedIntoAScopeBodyIsDropped();
    void aBlankLineTypedIntoAScopeBodyIsDropped();
    void aBlockBodyIsEditedUnderTheTable();
    void theBodyBoxIsOnlyForBlocks();
    void addingADirectivePutsItAfterTheSelectedRow();
    void addingABlockGivesItAnEditableBody();
    void thereIsNoWayToAddAComment();
    void thereIsNoWayToAddABlankLine();
    void anEntryCannotBeLeftWithoutAName();
    void removingARowRemovesOnlyThatEntry();
    void movingARowMovesOnlyThatEntry();
    void everyEditIsReported();
    void theMoveButtonsStopAtTheEnds();
};

void Openvpn3DirectivesTest::everyLineOfTheProfileIsARow()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    // Fourteen lines of text, seven of which are entries: the certificate is
    // one block rather than five rows, and the comment and the blank lines are
    // no rows at all.
    QCOMPARE(table(&widget)->rowCount(), 7);
    QCOMPARE(namesIn(table(&widget)),
             QStringList({u"client"_s, u"remote"_s, u"remote"_s, u"push-peer-info"_s, u"setenv"_s, u"ca"_s,
                          u"some-directive-we-have-never-heard-of"_s}));
}

void Openvpn3DirectivesTest::anUntouchedProfileComesBackUnchanged()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    // Looking at it is not editing it: the odd quoting and the block come back
    // byte for byte.  The formatting does not come back at all, and that is
    // the one thing loading a profile changes.
    QCOMPARE(widget.profile().toText(), kProfileKept);
}

void Openvpn3DirectivesTest::repeatedDirectivesAreNeitherMergedNorReordered()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    QCOMPARE(rowOf(table(&widget), u"remote"_s, 0), 1);
    QCOMPARE(rowOf(table(&widget), u"remote"_s, 1), 2);
    QCOMPARE(table(&widget)->item(1, ArgumentsColumn)->text(), u"a.example.org 1194 udp"_s);
    QCOMPARE(table(&widget)->item(2, ArgumentsColumn)->text(), u"b.example.org 443 tcp"_s);

    // Editing the second one is editing the second one.
    table(&widget)->item(2, ArgumentsColumn)->setText(u"c.example.org 443 tcp"_s);

    const QString text = widget.profile().toText();
    QVERIFY(text.contains(u"remote a.example.org 1194 udp\nremote c.example.org 443 tcp\n"_s));
}

void Openvpn3DirectivesTest::editingOneDirectiveLeavesTheRestAlone()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    const int row = rowOf(table(&widget), u"some-directive-we-have-never-heard-of"_s);
    table(&widget)->item(row, ArgumentsColumn)->setText(u"4 5 6"_s);

    QCOMPARE(widget.profile().toText(),
             QString(kProfileKept).replace(u"some-directive-we-have-never-heard-of 1 2 3"_s, u"some-directive-we-have-never-heard-of 4 5 6"_s));
}

void Openvpn3DirectivesTest::editingADirectivesArgumentsRespectsQuoting()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    const int row = rowOf(table(&widget), u"setenv"_s);
    // Shown with quotes the user can see and change.
    QCOMPARE(table(&widget)->item(row, ArgumentsColumn)->text(), u"opt \"single quoted value\""_s);

    table(&widget)->item(row, ArgumentsColumn)->setText(u"opt \"a different value\""_s);

    QCOMPARE(Openvpn3Profile::fromText(widget.profile().toText()).arguments(u"setenv"_s), QStringList({u"opt"_s, u"a different value"_s}));
}

void Openvpn3DirectivesTest::renamingADirectiveKeepsItsArguments()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\nremote a.example.org 1194 udp\n"_s));

    table(&widget)->item(1, NameColumn)->setText(u"remote-random-hostname"_s);

    QCOMPARE(widget.profile().toText(), u"client\nremote-random-hostname a.example.org 1194 udp\n"_s);
}

void Openvpn3DirectivesTest::commentsAreNotRowsAtAll()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    // The comment the profile was loaded with is not a row to be found, and
    // no row is of a kind the table used to call "Comment".
    QCOMPARE(table(&widget)->item(0, NameColumn)->text(), u"client"_s);
    for (int row = 0; row < table(&widget)->rowCount(); ++row) {
        QVERIFY(table(&widget)->item(row, KindColumn)->text() != u"Comment"_s);
        QVERIFY(!table(&widget)->item(row, ArgumentsColumn)->text().startsWith(u'#'));
    }
    // And it is not waiting in the profile to be written back out either.
    QVERIFY(!widget.profile().toText().contains(u"# a comment"_s));
    QVERIFY(!widget.profile().toText().contains(u"# the fallback"_s));
}

void Openvpn3DirectivesTest::blankLinesAreNotRowsAtAll()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    // The two blank lines the profile was loaded with are not rows, and no row
    // is of a kind the table used to call "Blank line": every row has a name
    // and a kind that can be acted on.
    for (int row = 0; row < table(&widget)->rowCount(); ++row) {
        QVERIFY(table(&widget)->item(row, KindColumn)->text() != u"Blank line"_s);
        QVERIFY(!table(&widget)->item(row, NameColumn)->text().isEmpty());
        QVERIFY(table(&widget)->item(row, KindColumn)->text() == u"Directive"_s || table(&widget)->item(row, KindColumn)->text() == u"Block"_s);
    }
    // And they are not waiting in the profile to be written back out either.
    QVERIFY(!widget.profile().toText().contains(u"\n\n"_s));
    QCOMPARE(widget.profile().toText(), kProfileKept);
}

void Openvpn3DirectivesTest::aCommentTypedIntoAScopeBodyIsDropped()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\n<connection>\nremote a.example.org\n</connection>\n"_s));

    const int row = rowOf(table(&widget), u"connection"_s);
    table(&widget)->selectRow(row);
    body(&widget)->setPlainText(u"# mine\nremote b.example.org 443 tcp # here\n"_s);

    // A <connection> body is the scope's directives, so a comment typed into
    // it has nowhere to live either: keeping it would save an edit that
    // vanishes the next time the connection is loaded.
    QCOMPARE(widget.profile().toText(), u"client\n<connection>\nremote b.example.org 443 tcp\n</connection>\n"_s);
    QCOMPARE(table(&widget)->item(row, ArgumentsColumn)->text(), u"1 line"_s);

    // An opaque payload is content rather than directives and keeps every
    // byte of what is typed into it.
    widget.setProfile(Openvpn3Profile::fromText(u"client\n<ca>\nPEM\n</ca>\n"_s));
    const int caRow = rowOf(table(&widget), u"ca"_s);
    table(&widget)->selectRow(caRow);
    body(&widget)->setPlainText(u"# payload\nA;B#C\n"_s);
    QCOMPARE(widget.profile().toText(), u"client\n<ca>\n# payload\nA;B#C\n</ca>\n"_s);
}

void Openvpn3DirectivesTest::aBlankLineTypedIntoAScopeBodyIsDropped()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\n<connection>\nremote a.example.org\n</connection>\n"_s));

    const int row = rowOf(table(&widget), u"connection"_s);
    table(&widget)->selectRow(row);
    body(&widget)->setPlainText(u"\nremote b.example.org 443 tcp\n   \n"_s);

    // A <connection> body is the scope's directives, so a blank line typed
    // into it has nowhere to live either, and the line count the table shows
    // is the number of entries the scope ended up with.
    QCOMPARE(widget.profile().toText(), u"client\n<connection>\nremote b.example.org 443 tcp\n</connection>\n"_s);
    QCOMPARE(table(&widget)->item(row, ArgumentsColumn)->text(), u"1 line"_s);

    // An opaque payload is content rather than directives and keeps every
    // blank line typed into it, at either end included.
    widget.setProfile(Openvpn3Profile::fromText(u"client\n<ca>\nPEM\n</ca>\n"_s));
    const int caRow = rowOf(table(&widget), u"ca"_s);
    table(&widget)->selectRow(caRow);
    body(&widget)->setPlainText(u"\nPEM\n\n"_s);
    QCOMPARE(widget.profile().toText(), u"client\n<ca>\n\nPEM\n\n</ca>\n"_s);
    QCOMPARE(table(&widget)->item(caRow, ArgumentsColumn)->text(), u"3 lines"_s);
}

void Openvpn3DirectivesTest::aBlockBodyIsEditedUnderTheTable()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    const int row = rowOf(table(&widget), u"ca"_s);
    table(&widget)->selectRow(row);

    QVERIFY(body(&widget)->isEnabled());
    QVERIFY(body(&widget)->toPlainText().contains(u"BEGIN CERTIFICATE"_s));
    // The table shows how much is in there rather than the certificate itself.
    QCOMPARE(table(&widget)->item(row, ArgumentsColumn)->text(), u"3 lines"_s);

    body(&widget)->setPlainText(u"REPLACED\n"_s);

    QVERIFY(widget.profile().toText().contains(u"<ca>\nREPLACED\n</ca>\n"_s));
    QCOMPARE(table(&widget)->item(row, ArgumentsColumn)->text(), u"1 line"_s);
}

void Openvpn3DirectivesTest::theBodyBoxIsOnlyForBlocks()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    table(&widget)->selectRow(rowOf(table(&widget), u"client"_s));
    QVERIFY(!body(&widget)->isEnabled());
    QVERIFY(body(&widget)->toPlainText().isEmpty());

    table(&widget)->selectRow(rowOf(table(&widget), u"ca"_s));
    QVERIFY(body(&widget)->isEnabled());

    // Typing in it while a directive is selected cannot reach the document.
    table(&widget)->selectRow(rowOf(table(&widget), u"client"_s));
    body(&widget)->setPlainText(u"nowhere\n"_s);
    QCOMPARE(widget.profile().toText(), kProfileKept);
}

void Openvpn3DirectivesTest::addingADirectivePutsItAfterTheSelectedRow()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\nremote a.example.org\n"_s));

    table(&widget)->selectRow(0);
    button(&widget, u"openvpn3_directives_add"_s)->click();

    QCOMPARE(table(&widget)->rowCount(), 3);
    QCOMPARE(table(&widget)->currentRow(), 1);
    // Order is meaningful to OpenVPN, so a new entry goes where the user was,
    // not at the end of the file.
    QCOMPARE(namesIn(table(&widget)), QStringList({u"client"_s, u"directive"_s, u"remote"_s}));

    table(&widget)->item(1, NameColumn)->setText(u"comp-lzo"_s);
    table(&widget)->item(1, ArgumentsColumn)->setText(u"no"_s);
    QCOMPARE(widget.profile().toText(), u"client\ncomp-lzo no\nremote a.example.org\n"_s);
}

void Openvpn3DirectivesTest::addingABlockGivesItAnEditableBody()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\nremote a.example.org\n"_s));

    button(&widget, u"openvpn3_directives_add_block"_s)->click();
    const int row = table(&widget)->currentRow();
    table(&widget)->item(row, NameColumn)->setText(u"tls-crypt"_s);
    QVERIFY(body(&widget)->isEnabled());
    body(&widget)->setPlainText(u"KEY-MATERIAL\n"_s);

    QCOMPARE(widget.profile().toText(), u"client\nremote a.example.org\n<tls-crypt>\nKEY-MATERIAL\n</tls-crypt>\n"_s);
}

void Openvpn3DirectivesTest::thereIsNoWayToAddAComment()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\n"_s));

    // Offering a button for something the profile cannot keep would only
    // promise an edit that vanishes the next time the connection is loaded.
    QVERIFY(!button(&widget, u"openvpn3_directives_add_comment"_s));
    QVERIFY(button(&widget, u"openvpn3_directives_add"_s));
    QVERIFY(button(&widget, u"openvpn3_directives_add_block"_s));
}

void Openvpn3DirectivesTest::thereIsNoWayToAddABlankLine()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\n"_s));

    // Same reason: a blank line is not an entry the profile keeps, so a button
    // for one would only promise an edit that vanishes on the next load.
    QVERIFY(!button(&widget, u"openvpn3_directives_add_blank"_s));
    QCOMPARE(widget.findChildren<QPushButton *>().size(), 5); // add, add block, remove, up, down
}

void Openvpn3DirectivesTest::anEntryCannotBeLeftWithoutAName()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\nremote a.example.org 1194 udp\n"_s));
    QSignalSpy changed(&widget, &Openvpn3DirectivesWidget::changed);

    // A directive with no name renders as a blank line, which the next load
    // would drop -- the row would be gone and its arguments with it. Clearing
    // the name is therefore no edit at all: the cell goes back to what it was.
    table(&widget)->item(1, NameColumn)->setText(QString());

    QCOMPARE(table(&widget)->item(1, NameColumn)->text(), u"remote"_s);
    QCOMPARE(table(&widget)->rowCount(), 2);
    QCOMPARE(widget.profile().toText(), u"client\nremote a.example.org 1194 udp\n"_s);
    QVERIFY(changed.isEmpty());

    // Whitespace is not a name either.
    table(&widget)->item(1, NameColumn)->setText(u"   "_s);
    QCOMPARE(widget.profile().toText(), u"client\nremote a.example.org 1194 udp\n"_s);
    QVERIFY(changed.isEmpty());
}

void Openvpn3DirectivesTest::removingARowRemovesOnlyThatEntry()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    table(&widget)->selectRow(rowOf(table(&widget), u"remote"_s, 0));
    button(&widget, u"openvpn3_directives_remove"_s)->click();

    QCOMPARE(table(&widget)->rowCount(), 6);
    QCOMPARE(widget.profile().toText(), QString(kProfileKept).replace(u"remote a.example.org 1194 udp\n"_s, QString()));
}

void Openvpn3DirectivesTest::movingARowMovesOnlyThatEntry()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));

    const int row = rowOf(table(&widget), u"push-peer-info"_s);
    table(&widget)->selectRow(row);
    button(&widget, u"openvpn3_directives_up"_s)->click();

    QCOMPARE(table(&widget)->currentRow(), row - 1);
    QVERIFY(widget.profile().toText().contains(u"push-peer-info\nremote b.example.org 443 tcp\n"_s));
    // And nothing else changed places.
    QCOMPARE(widget.profile().toText(),
             QString(kProfileKept).replace(u"remote b.example.org 443 tcp\npush-peer-info\n"_s, u"push-peer-info\nremote b.example.org 443 tcp\n"_s));
}

void Openvpn3DirectivesTest::everyEditIsReported()
{
    // The page above this one turns changed() into the signal the connection
    // editor enables its Save button on, so an edit that does not report is an
    // edit that cannot be saved.
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(kProfile));
    QSignalSpy changed(&widget, &Openvpn3DirectivesWidget::changed);

    table(&widget)->item(rowOf(table(&widget), u"client"_s), NameColumn)->setText(u"pull"_s);
    QCOMPARE(changed.size(), 1);

    table(&widget)->item(rowOf(table(&widget), u"setenv"_s), ArgumentsColumn)->setText(u"opt other"_s);
    QCOMPARE(changed.size(), 2);

    table(&widget)->selectRow(rowOf(table(&widget), u"ca"_s));
    body(&widget)->setPlainText(u"OTHER\n"_s);
    QCOMPARE(changed.size(), 3);

    button(&widget, u"openvpn3_directives_add"_s)->click();
    QCOMPARE(changed.size(), 4);

    button(&widget, u"openvpn3_directives_remove"_s)->click();
    QCOMPARE(changed.size(), 5);

    table(&widget)->selectRow(1);
    button(&widget, u"openvpn3_directives_down"_s)->click();
    QCOMPARE(changed.size(), 6);
}

void Openvpn3DirectivesTest::theMoveButtonsStopAtTheEnds()
{
    Openvpn3DirectivesWidget widget;
    widget.setProfile(Openvpn3Profile::fromText(u"client\nremote a.example.org\n"_s));

    table(&widget)->selectRow(0);
    QVERIFY(!button(&widget, u"openvpn3_directives_up"_s)->isEnabled());
    QVERIFY(button(&widget, u"openvpn3_directives_down"_s)->isEnabled());

    table(&widget)->selectRow(1);
    QVERIFY(button(&widget, u"openvpn3_directives_up"_s)->isEnabled());
    QVERIFY(!button(&widget, u"openvpn3_directives_down"_s)->isEnabled());
}

QTEST_MAIN(Openvpn3DirectivesTest)

#include "openvpn3directivestest.moc"
