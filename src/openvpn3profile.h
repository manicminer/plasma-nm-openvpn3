/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#ifndef PLASMA_NM_OPENVPN3_PROFILE_H
#define PLASMA_NM_OPENVPN3_PROFILE_H

#include <QList>
#include <QString>
#include <QStringList>

/**
 * One line (or one @c <tag> block) of an OpenVPN profile.
 *
 * Entries keep the source they were parsed from. An entry that nothing has
 * changed is written back exactly as it came in, which is what makes a round
 * trip lossless: unknown directives, odd quoting, duplicate remotes and
 * @c <connection> blocks all survive it even though the editor has no idea
 * what they mean.
 *
 * There is no comment entry and no blank entry: neither is something openvpn3
 * reads, and a table row for one would be a row nothing can act on. They are
 * kept all the same, carried by the entry they precede -- and a comment at the
 * end of a directive's line by that directive -- so that the document comes
 * back out of Openvpn3Profile::toText() with every one of them in place.
 */
class Openvpn3Entry
{
public:
    enum Kind {
        Directive, //!< @c name [arguments...]
        Block, //!< @c <name> ... @c </name>, body kept verbatim
    };

    Kind kind = Directive;
    //! Directive or block name.
    QString name;
    //! Arguments with their quoting removed.
    QStringList arguments;
    //! Block body, verbatim, including the newline of its last line.
    QString body;

    bool isDirective() const
    {
        return kind == Directive;
    }
    bool isBlock() const
    {
        return kind == Block;
    }

    /**
     * What makes this entry this entry, whatever happens around it.
     *
     * The server table remembers which row is which entry across edits that
     * insert, remove and reorder entries, and a position cannot do that: it
     * would hand one remote's trailing arguments to another. An id is unique
     * for the lifetime of the process and travels with copies of the entry,
     * so an entry taken out of a profile and put back is still itself.
     */
    quint64 id() const
    {
        return m_id;
    }

    //! The entry's first argument, the common case for a single-value directive.
    QString value() const
    {
        return arguments.value(0);
    }

private:
    friend class Openvpn3Profile;
    //! Hands out one id, and never the same one twice.
    static quint64 nextId();

    //! The comment and blank lines in front of this entry, verbatim, each
    //! with its terminator. Formatting, which travels with the entry.
    QString m_leading;
    //! The source this entry was parsed from, verbatim: the comment at the
    //! end of its line and its terminator included. Never blank.
    QString m_source;
    //! The comment at the end of the entry's (opening) line, with the
    //! whitespace in front of it and without the terminator; kept when the
    //! entry itself is rewritten.
    QString m_comment;
    //! The terminator of the entry's last line: "\n", "\r\n", or empty for
    //! a final line with none. Only meaningful when m_parsed.
    QString m_terminator;
    //! True for an entry read from text, false for one made here.
    bool m_parsed = false;
    //! False once something changed it; then it is rendered instead.
    bool m_verbatim = false;
    quint64 m_id = nextId();
};

/**
 * An OpenVPN profile as an ordered list of entries.
 *
 * The order and multiplicity of entries is part of the document: OpenVPN cares
 * about the order of @c remote directives, and repeating a directive is
 * meaningful. Nothing is deduplicated, sorted or normalised, and there is no
 * list of directives the editor knows about -- an entry it does not understand
 * is simply an entry it does not touch.
 *
 * One lossless document: toText() of what fromText() read is the text it was
 * given, byte for byte -- comments, blank lines, CRLF and a missing final
 * newline included. The formatting is not among the entries, so the table
 * built from them shows none of it; it rides along with the entries instead,
 * and the editing methods below say what happens to it.
 */
class Openvpn3Profile
{
public:
    /**
     * Parses @p text, losing nothing.
     *
     * Directive order, repeated directives, quoting, the whitespace a line is
     * padded with, whatever @c <tag> blocks the profile has, and the
     * formatting -- comment lines, blank lines and a comment at the end of a
     * directive -- all come back out of toText(). A line that is nothing but
     * a comment, or nothing but whitespace, becomes no entry: it is carried by
     * the entry after it, or after the last entry when nothing follows.
     *
     * A @c # or @c ; starts a comment when it is unquoted, unescaped and at
     * the start of a word -- what OpenVPN 2's @c parse_line() and openvpn3's
     * @c OptionList::LexComment agree on. One inside quotes, escaped with a
     * backslash or glued to the middle of a word is part of a value and stays.
     *
     * A line is blank when it holds nothing but ASCII whitespace other than
     * @c \v, which is what the backend's importer strips a line down to; see
     * @c isBlankLine() in the implementation. A line of @c U+00A0 or of
     * @c \v is a value rather than formatting.
     *
     * The body of a block is kept verbatim and not looked into: the lines of
     * an opaque payload (@c <ca>, @c <key>, an inline @c <auth-user-pass>, an
     * unknown @c <tag>) are content, and a @c <connection> scope's lines,
     * formatting and all, are exactly what the backend keeps of it too.
     */
    static Openvpn3Profile fromText(const QString &text);

    /** The profile text: byte for byte the input of fromText() where nothing
     * was changed, and only changed entries re-rendered. */
    QString toText() const;

    /** The document as openvpn3 reads it: every entry, with the formatting
     * -- comment and blank lines, comments at the end of a line, and those
     * inside a @c <connection> scope -- left out. For looking at, and for
     * comparing two documents by what they mean; never what is stored. */
    QString optionsText() const;

    bool isEmpty() const
    {
        return m_entries.isEmpty();
    }
    int count() const
    {
        return m_entries.count();
    }
    const QList<Openvpn3Entry> &entries() const
    {
        return m_entries;
    }
    const Openvpn3Entry &at(int index) const
    {
        return m_entries.at(index);
    }

    //! Indexes of every directive or block called @p name, in document order.
    QList<int> indexesOf(const QString &name) const;
    //! The index of the first one, or -1.
    int indexOf(const QString &name) const;
    bool contains(const QString &name) const
    {
        return indexOf(name) >= 0;
    }
    //! The index of the entry with Openvpn3Entry::id() @p id, or -1.
    int indexOfId(quint64 id) const;
    //! First argument of the first @p name directive, or an empty string.
    QString value(const QString &name) const;
    //! Arguments of the first @p name directive.
    QStringList arguments(const QString &name) const;
    //! Body of the first @p name block, or an empty string.
    QString blockBody(const QString &name) const;
    //! The profile text of one entry, terminator included.
    QString sourceAt(int index) const;

    /**
     * Every entry openvpn3 itself reads as an option.
     *
     * The top level is not all of it: @c &lt;connection&gt; holds options
     * rather than a payload, and a client profile may well keep its only
     * remote and the files that remote needs inside one. Everything else in
     * angle brackets -- @c &lt;ca&gt;, @c &lt;key&gt;, an inline
     * @c &lt;auth-user-pass&gt; -- is opaque content that is reported as the
     * one entry it is and never descended into, so a certificate whose base64
     * begins with a word this editor knows is still just a certificate.
     *
     * This is for looking, not for editing: the indexes of the editing methods
     * address the top level, which is the only place they change anything.
     * What openvpn3 actually uses from a scope is narrower; see
     * connectionCaveats().
     */
    QList<Openvpn3Entry> optionEntries() const;
    //! Entries called @p name anywhere optionEntries() reaches, in order.
    QList<Openvpn3Entry> optionsNamed(const QString &name) const;
    //! True when @p name is an option anywhere optionEntries() reaches.
    bool containsOption(const QString &name) const
    {
        return !optionsNamed(name).isEmpty();
    }

    /** The host of every @c remote, in every scope and in document order.
     * Blank hosts are listed: whether one is good enough is the editor's
     * question, not the document's. */
    QStringList remoteHosts() const;

    /**
     * True when this profile is not self-contained at its top level.
     *
     * openvpn3 is given one profile and nothing else, so a profile that points
     * at a file, or that carries credentials inline, has to go through the
     * backend's importer before it can be stored. This only decides whether
     * to ask; what the answer looks like is the backend's to say.
     *
     * Only the top level counts, because only the top level is what the
     * backend resolves: a reference inside a @c <connection> scope is kept as
     * written by the import, and openvpn3 does not read it. See
     * connectionCaveats().
     */
    bool needsNormalization() const;
    //! Whether the top-level entry at @p index is one needsNormalization() means.
    bool needsNormalizationAt(int index) const;

    /** Something about a @c <connection> scope the user should know, because
     * openvpn3 will not do with it what OpenVPN 2 would. */
    struct ConnectionCaveat {
        enum Kind {
            //! An option openvpn3 does not read from a scope: only
            //! @c remote, @c proto and @c port are taken from one.
            IgnoredOption,
            //! A file reference, which the backend does not inline in a scope.
            FileReference,
            //! The scope is past the size openvpn3 accepts for one.
            TooLarge,
        };
        Kind kind;
        //! Which @c <connection> block, counting from 1.
        int block = 0;
        //! The option, for IgnoredOption and FileReference.
        QString name;
        //! The file named, for FileReference; the estimated size, for TooLarge.
        QString detail;
    };
    /**
     * What openvpn3 will not do with this profile's @c <connection> scopes.
     *
     * openvpn3's RemoteList (openvpn/client/remotelist.hpp) parses each scope
     * as an option list of its own, takes @c remote, @c proto and @c port from
     * it and nothing else, and refuses a scope whose accounted size -- its
     * text, comments included, plus 64 bytes per option and 16 per argument --
     * reaches 4096. None of that is a reason to change the document: the scope
     * means what it says to OpenVPN 2, and the backend keeps it as written. It
     * is a reason to say so.
     */
    QList<ConnectionCaveat> connectionCaveats() const;
    /**
     * The name of the first block whose tags the backend's importer reads
     * differently from openvpn3, or an empty string when they agree.
     *
     * openvpn3 opens a block on a line whose one lexed word is a tag and
     * closes it only on a line that is exactly the closing tag. The importer
     * (ovpn-import.c) compares the line stripped of whitespace instead: it
     * does not see an opening tag that is quoted or has a comment after it,
     * and it does see an indented closing tag. Where they disagree, the
     * importer would read what openvpn3 holds as a scope or a payload as
     * directives -- and inline the files or lift the credentials in it.
     */
    QString importerTagDisagreement() const;
    /** openvpn3's accounting for one @c <connection> body; see
     * connectionCaveats(). */
    static int connectionBlockCost(const QString &body);
    //! The size at and above which openvpn3 refuses a @c <connection> block.
    static constexpr int kMaxConnectionBlockCost = 4096;

    //! True when the document's lines end in CRLF rather than LF.
    bool usesCrlf() const;

    /**
     * True when the profile carries key material a passphrase could unlock,
     * at the top level: openvpn3 reads none from a @c <connection> block.
     *
     * An unencrypted PEM key needs none, so asking for one would be a field
     * nothing ever reads. A PKCS#12 bundle or a key this profile only points
     * at cannot be inspected from here, so those count as "could".
     */
    bool mayNeedPrivateKeyPassphrase() const;

    /** Replaces the arguments of one entry and nothing else. The line is
     * re-rendered; the comment at its end and the formatting in front of it
     * stay. */
    void setArguments(int index, const QStringList &arguments);
    /** Replaces the body of one block and nothing else, byte for byte; in a
     * document with CRLF lines the body's lines get CRLF too. */
    void setBody(int index, const QString &body);
    /** Renames one entry, keeping its arguments or body, its comment and the
     * formatting in front of it. */
    void rename(int index, const QString &name);
    /** Puts @p entry where entry @p index is. The formatting goes with the
     * entries: what @p entry carries comes with it, and what the old entry
     * carried goes with that one. */
    void replace(int index, const Openvpn3Entry &entry);
    /**
     * Replaces entry @p index with @p entries, in its place.
     *
     * The formatting in front of the old entry goes in front of the first
     * new one, and a comment at the end of its line becomes a comment line of
     * its own in front of them, so that it still says what it said. With no
     * entries at all this is removeAt().
     */
    void replaceWith(int index, const QList<Openvpn3Entry> &entries);
    /** Removes one entry. The comment and blank lines in front of it stay in
     * the document, in front of whatever follows, and a comment at the end of
     * its line stays as a comment line of its own where the line was. */
    void removeAt(int index);
    void removeAll(const QString &name);
    /** Appends @p entry after the last entry, in front of any formatting the
     * document ends with. */
    int append(const Openvpn3Entry &entry);
    /** Inserts @p entry in front of entry @p index and the formatting that
     * entry carries. */
    void insert(int index, const Openvpn3Entry &entry);
    /** Moves one entry; see reorder() for what happens to the formatting. */
    void move(int from, int to);
    /**
     * Puts the entries with the ids @p ids, in that order, in the positions
     * @p places, which they occupy between them now.
     *
     * The formatting is split the way a reader would: the comment lines
     * directly in front of an entry are about it and go where it goes, while
     * a blank line, and anything in front of one, belongs to the place in the
     * document and stays there for whichever entry ends up in it.
     */
    void reorder(const QList<int> &places, const QList<quint64> &ids);

    /** Sets the arguments of the first @p name directive, appending it when
     * there is none. Later duplicates are left alone: they are the user's. */
    void setDirective(const QString &name, const QStringList &arguments);
    /** Adds a bare @p name directive if missing, removes every one of them if
     * @p present is false. An existing directive keeps its arguments. */
    void setPresent(const QString &name, bool present);
    //! Sets the body of the first @p name block, appending the block if missing.
    void setBlock(const QString &name, const QString &body);

    static Openvpn3Entry directive(const QString &name, const QStringList &arguments = {});
    //! A @c <name> block with @p body, verbatim.
    static Openvpn3Entry block(const QString &name, const QString &body);

    /** Splits a directive line the way openvpn3 does (Split::by_space over
     * OptionList::LexComment): a backslash escapes the next character
     * everywhere, quotes group, an unquoted, unescaped @c # or @c ; starts a
     * comment anywhere, and escaped whitespace still separates. */
    static QStringList splitArguments(const QString &line);
    //! Quotes @p argument only when it would not survive splitArguments().
    static QString quoteArgument(const QString &argument);

private:
    //! The line terminator of the document: the first one it has, or LF.
    QString newline() const;
    QString textOf(const Openvpn3Entry &entry, const QString &newline) const;
    static QString render(const Openvpn3Entry &entry, const QString &newline);

    QList<Openvpn3Entry> m_entries;
    //! Comment and blank lines after the last entry, verbatim.
    QString m_trailing;
};

#endif // PLASMA_NM_OPENVPN3_PROFILE_H
