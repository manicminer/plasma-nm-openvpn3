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
 * trip lossless but for the formatting: unknown directives, odd quoting,
 * duplicate remotes and @c <connection> blocks all survive it even though the
 * editor has no idea what they mean.
 *
 * There is no comment entry and no blank entry. Neither is something openvpn3
 * reads or this editor can do anything with, so Openvpn3Profile::fromText()
 * drops both rather than keep a row nothing can act on; see that function.
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

    //! The source this entry was parsed from, minus any comment on it,
    //! terminator included. Never blank: a blank line is no entry.
    QString m_source;
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
 * The formatting -- comments and blank lines -- is the one thing this does not
 * keep, and the one thing reading a profile changes about it; see fromText().
 */
class Openvpn3Profile
{
public:
    /**
     * Parses @p text, dropping the formatting: the comments and the blank
     * lines.
     *
     * Everything else is kept: directive order, repeated directives, quoting,
     * the whitespace a directive line is padded with, and whatever @c <tag>
     * blocks the profile has. A line that is nothing but a comment, or nothing
     * but whitespace, becomes no entry at all, and a comment after a directive
     * is cut off the entry's source, so none of it can come back out of
     * toText(). A profile that was only ever formatting parses to an empty
     * document.
     *
     * A @c # or @c ; starts a comment when it is unquoted, unescaped and at
     * the start of a word -- what OpenVPN 2's @c parse_line() and openvpn3's
     * @c OptionList::LexComment agree on. One inside quotes, escaped with a
     * backslash or glued to the middle of a word is part of a value and stays.
     *
     * A line is blank when it holds nothing but the whitespace the backend's
     * importer strips a line with; see @c isBlankLine() in the implementation.
     * A line of @c U+00A0 is a value rather than formatting, and so is one of
     * @c \\v, which @c g_ascii_isspace() does not count as whitespace.
     *
     * The lines of an opaque payload (@c <ca>, @c <key>, an inline
     * @c <auth-user-pass>, an unknown @c <tag>) are content rather than
     * formatting and are never inspected, so a blank line in a certificate or
     * an empty credential survives byte for byte. A @c <connection> scope
     * holds directives, so the formatting among those goes too, while a block
     * nested inside it stays opaque.
     *
     * Keep this in step with @c comment_start() and @c strip_separators() in
     * the backend's ovpn-import.c, which do the same to a profile on import.
     */
    static Openvpn3Profile fromText(const QString &text);

    /** The profile text. Byte for byte the input of fromText() but for the
     * formatting it dropped, and only changed entries are re-rendered. */
    QString toText() const;

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
     * Keep it in step with @c option_scopes in the backend's ovpn-import.c.
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
     * True when this profile is not self-contained.
     *
     * openvpn3 is given one profile and nothing else, so a profile that points
     * at a file, or that carries credentials inline, has to go through the
     * backend's normalizer before it can be stored. This only decides whether
     * to ask; what the answer looks like is the backend's to say.
     */
    bool needsNormalization() const;

    //! True when the document's lines end in CRLF rather than LF.
    bool usesCrlf() const;

    /**
     * True when the profile carries key material a passphrase could unlock.
     *
     * An unencrypted PEM key needs none, so asking for one would be a field
     * nothing ever reads. A PKCS#12 bundle or a key this profile only points
     * at cannot be inspected from here, so those count as "could".
     */
    bool mayNeedPrivateKeyPassphrase() const;

    //! Replaces the arguments of one entry and nothing else.
    void setArguments(int index, const QStringList &arguments);
    /** Replaces the body of one block and nothing else. A @c <connection>
     * body holds directives, so the formatting among them goes the way
     * fromText() takes it; any other body is opaque payload and is stored
     * byte for byte. */
    void setBody(int index, const QString &body);
    void replace(int index, const Openvpn3Entry &entry);
    void removeAt(int index);
    void removeAll(const QString &name);
    int append(const Openvpn3Entry &entry);
    void insert(int index, const Openvpn3Entry &entry);
    void move(int from, int to);

    /** Sets the arguments of the first @p name directive, appending it when
     * there is none. Later duplicates are left alone: they are the user's. */
    void setDirective(const QString &name, const QStringList &arguments);
    /** Adds a bare @p name directive if missing, removes every one of them if
     * @p present is false. An existing directive keeps its arguments. */
    void setPresent(const QString &name, bool present);
    //! Sets the body of the first @p name block, appending the block if missing.
    void setBlock(const QString &name, const QString &body);

    static Openvpn3Entry directive(const QString &name, const QStringList &arguments = {});
    //! A @c <name> block; its body is treated as setBody() treats one.
    static Openvpn3Entry block(const QString &name, const QString &body);

    /** Splits a directive line the way OpenVPN does: double quotes allow
     * backslash escapes, single quotes are literal, an unquoted @c # or @c ;
     * starts a comment. */
    static QStringList splitArguments(const QString &line);
    //! Quotes @p argument only when it would not survive splitArguments().
    static QString quoteArgument(const QString &argument);

private:
    static QString render(const Openvpn3Entry &entry);

    QList<Openvpn3Entry> m_entries;
};

#endif // PLASMA_NM_OPENVPN3_PROFILE_H
