/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "openvpn3profile.h"

#include <utility>

namespace
{

/** Splits @p text into lines that still carry their terminator, so that
 * joining them again reproduces the input exactly -- CRLF, a missing final
 * newline and all. */
QStringList linesWithTerminators(const QString &text)
{
    QStringList lines;
    int start = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == u'\n') {
            lines.append(text.mid(start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        lines.append(text.mid(start));
    }
    return lines;
}

/** The whitespace a directive line is made of words with.
 *
 * Only ASCII whitespace separates anything: that is all openvpn3's own lexer
 * breaks on, and all @c g_ascii_isspace() -- what the backend's ovpn-import.c
 * uses throughout -- counts. @c QChar::isSpace() also takes the Unicode
 * separators, @c U+00A0 and friends, which are literal bytes of the value
 * they sit in; treating one as a separator would delete it from a value or
 * let a @c # glued behind it pass for the start of a comment. */
bool isAsciiSpace(QChar c)
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\v' || c == u'\f' || c == u'\r';
}

/** A line of nothing: formatting rather than a directive, and therefore no
 * entry of the document at all.
 *
 * What counts as nothing is what the backend's importer strips a line down to
 * with @c g_strstrip(), which goes by @c g_ascii_isspace(). That is @c isAsciiSpace() above less @c '\\v', which
 * GLib -- unlike C's @c isspace() -- does not count: so a line of vertical
 * tabs is a value for the importer, and has to stay a value here too, or the
 * two disagree about which lines a profile has. Unicode separators are not
 * whitespace for either of them, so a line of @c U+00A0 is a value as well. */
bool isBlankLine(const QString &line)
{
    for (const QChar c : line) {
        if (c != u' ' && c != u'\t' && c != u'\n' && c != u'\f' && c != u'\r') {
            return false;
        }
    }
    return true;
}

bool isOpeningTag(const QString &stripped)
{
    return stripped.size() > 2 && stripped.startsWith(u'<') && stripped.endsWith(u'>') && !stripped.startsWith(QLatin1String("</"));
}

/** @c {</tag>} on a line of its own, the only thing openvpn3 closes a block
 * with. */
bool isClosingTag(const QString &stripped)
{
    return stripped.size() > 3 && stripped.startsWith(QLatin1String("</")) && stripped.endsWith(u'>');
}

/** Blocks that hold options rather than an opaque payload: the one openvpn3
 * parses as an option list of its own (RemoteList), and the one the backend's
 * importer keeps as written. */
bool isOptionScope(const QString &name)
{
    return name == QLatin1String("connection");
}

/**
 * Where the comment on @p line starts, or -1 if it has none: where openvpn3,
 * which is what reads the stored profile, stops reading the line.
 *
 * That is openvpn3's @c OptionList::LexComment (openvpn/common/options.hpp):
 * a @c # or @c ; outside quotes and not escaped by a backslash, anywhere in
 * the line -- glued to the middle of a word as well. A backslash escapes the
 * character after it everywhere, inside single quotes too, and a quote of
 * one kind does not end inside a quote of the other.
 *
 * OpenVPN 2 reads some of those lines differently (@c a#b is a literal value
 * for it, and a backslash inside single quotes is literal). The line is kept
 * as written either way; what an entry says, and what an edit writes back,
 * is what openvpn3 reads.
 */
int commentStart(const QString &line)
{
    bool single = false;
    bool dbl = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == u'\\') {
            ++i; // escapes whatever follows, if anything does
        } else if (c == u'"' && !single) {
            dbl = !dbl;
        } else if (c == u'\'' && !dbl) {
            single = !single;
        } else if ((c == u'#' || c == u';') && !single && !dbl) {
            return i;
        }
    }
    return -1;
}

/** Takes the trailing whitespace that separates off @p text, and no more:
 * backslash-escaped whitespace is part of the value in front of it. An even
 * number of backslashes before the whitespace are escaped backslashes and
 * leave it a separator again. */
void chopTrailingSeparators(QString &text)
{
    while (!text.isEmpty() && isAsciiSpace(text.back())) {
        int backslashes = 0;
        while (backslashes < text.size() - 1 && text.at(text.size() - 2 - backslashes) == u'\\') {
            ++backslashes;
        }
        if (backslashes % 2 != 0) {
            break; // escaped: part of the value, not a separator
        }
        text.chop(1);
    }
}

/** @p line split into its content and its terminator ("\n", "\r\n" or
 * nothing). */
std::pair<QString, QString> splitTerminator(const QString &line)
{
    if (line.endsWith(QLatin1String("\r\n"))) {
        return {line.chopped(2), QStringLiteral("\r\n")};
    }
    if (line.endsWith(u'\n')) {
        return {line.chopped(1), QStringLiteral("\n")};
    }
    return {line, QStringLiteral("")};
}

/** The leading formatting split where a reader would: everything up to and
 * including the last blank line belongs to the place in the document, and the
 * comment lines after it are about the entry that follows them. */
std::pair<QString, QString> splitFormatting(const QString &leading)
{
    const QStringList lines = linesWithTerminators(leading);
    int attached = lines.size();
    while (attached > 0 && !isBlankLine(lines.at(attached - 1))) {
        --attached;
    }
    return {lines.mid(0, attached).join(QString()), lines.mid(attached).join(QString())};
}

/** @p text with every line terminated by @p newline. */
QString withNewlines(const QString &text, const QString &newline)
{
    QString converted = text;
    converted.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    if (newline != QLatin1String("\n")) {
        converted.replace(u'\n', newline);
    }
    return converted;
}

/** Directives naming a file the backend inlines when it normalises. Its
 * file_directives, plus pkcs12, which it inlines as base64. */
bool isFileDirective(const QString &name)
{
    static const QStringList names{
        QStringLiteral("ca"),
        QStringLiteral("cert"),
        QStringLiteral("key"),
        QStringLiteral("extra-certs"),
        QStringLiteral("tls-auth"),
        QStringLiteral("tls-crypt"),
        QStringLiteral("tls-crypt-v2"),
        QStringLiteral("crl-verify"),
        QStringLiteral("pkcs12"),
    };
    return names.contains(name);
}

}

quint64 Openvpn3Entry::nextId()
{
    static quint64 counter = 0;
    return ++counter;
}

QStringList Openvpn3Profile::splitArguments(const QString &line)
{
    // openvpn3's Split::by_space() over LexComment, character for character:
    // a backslash takes the next character as it is (and an escaped space
    // still separates, because by_space only asks whether the character it
    // was handed is a space), quotes group without being kept, a comment ends
    // the line, and a quote opened and closed makes an argument even when
    // nothing was between them.
    QStringList words;
    QString term;
    bool defined = false;
    bool single = false;
    bool dbl = false;
    for (int i = 0; i < line.size(); ++i) {
        QChar c = line.at(i);
        if (c == u'\\') {
            if (++i >= line.size()) {
                break; // nothing to escape
            }
            c = line.at(i);
        } else if (c == u'"' && !single) {
            dbl = !dbl;
            defined = true;
            continue;
        } else if (c == u'\'' && !dbl) {
            single = !single;
            defined = true;
            continue;
        } else if ((c == u'#' || c == u';') && !single && !dbl) {
            break;
        }
        if (!isAsciiSpace(c) || single || dbl) {
            term += c;
            defined = true;
        } else if (defined) {
            words.append(term);
            term.clear();
            defined = false;
        }
    }
    if (defined) {
        words.append(term);
    }
    return words;
}

QString Openvpn3Profile::quoteArgument(const QString &argument)
{
    bool needsQuotes = argument.isEmpty();
    for (const QChar c : argument) {
        // Quoting asks what could be lost, not what separates: Unicode
        // whitespace separates nothing, but a value that ends in it comes
        // back short of it, because reading a line strips it off along with
        // the terminator. So @c QChar::isSpace() here, where @c isAsciiSpace()
        // reads the line.
        if (c.isSpace() || c == u'"' || c == u'\'' || c == u'\\' || c == u'#' || c == u';') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return argument;
    }

    QString quoted;
    quoted.reserve(argument.size() + 2);
    quoted += u'"';
    for (const QChar c : argument) {
        if (c == u'"' || c == u'\\') {
            quoted += u'\\';
        }
        quoted += c;
    }
    quoted += u'"';
    return quoted;
}

Openvpn3Profile Openvpn3Profile::fromText(const QString &text)
{
    Openvpn3Profile profile;
    const QStringList lines = linesWithTerminators(text);
    // Comment and blank lines read so far, waiting for the entry they are in
    // front of.
    QString formatting;

    for (int i = 0; i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        // Formatting is not an entry: a line that is only a comment or only
        // whitespace is held for the next entry. The lines of a <tag> payload
        // are not read as lines at all -- the block below swallows them
        // whole, so a blank line in a certificate or an empty credential is
        // never mistaken for formatting.
        if (isBlankLine(raw)) {
            formatting += raw;
            continue;
        }
        const auto [content, terminator] = splitTerminator(raw);
        QString code = content;
        QString comment;
        const int cut = commentStart(content);
        if (cut >= 0) {
            QString kept = content.left(cut);
            chopTrailingSeparators(kept);
            if (kept.isEmpty()) {
                formatting += raw;
                continue;
            }
            // Cutting a comment must not turn a line into a closing tag the
            // raw line was not one: openvpn3 matches every closing tag
            // against the raw line, so {</connection> # x} is no boundary for
            // it even when a real closer follows below. Such a line is kept
            // whole as a directive -- the comment stays part of it, and
            // openvpn3 ignores it where it is. Keep this in step with the
            // backend's ovpn-import.c.
            if (!isClosingTag(kept.trimmed())) {
                code = kept;
                comment = content.mid(kept.size());
            }
        }

        // openvpn3 opens a block on a line whose one lexed word is a tag, so
        // quotes and padding around it do not matter.
        const QStringList words = splitArguments(code);
        Openvpn3Entry entry;
        entry.m_parsed = true;
        entry.m_verbatim = true;
        entry.m_leading = formatting;
        entry.m_comment = comment;
        entry.m_terminator = terminator;
        formatting.clear();

        if (words.size() == 1 && isOpeningTag(words.first())) {
            entry.kind = Openvpn3Entry::Block;
            entry.name = words.first().mid(1, words.first().size() - 2);
            const QString closing = QLatin1String("</") + entry.name + QLatin1Char('>');
            QString body;
            int end = i + 1;
            for (; end < lines.size(); ++end) {
                // openvpn3 matches a closing tag against the raw line, its
                // terminator aside (is_close_tag()): neither padding nor a
                // comment around one closes the block for it.
                if (splitTerminator(lines.at(end)).first == closing) {
                    break;
                }
                body += lines.at(end);
            }
            entry.body = body;
            if (end < lines.size()) {
                entry.m_source = raw + body + lines.at(end);
                entry.m_terminator = splitTerminator(lines.at(end)).second;
                i = end;
            } else {
                // Unterminated: everything that is left belongs to this block
                // and is kept exactly as it is rather than repaired -- a scope
                // whose closing tag carries a comment is not closed for
                // openvpn3 either, and repairing it here would invent one.
                entry.m_source = raw + body;
                entry.m_terminator = splitTerminator(lines.last()).second;
                i = lines.size() - 1;
            }
        } else {
            entry.m_source = raw;
            entry.name = words.value(0);
            entry.arguments = words.mid(1);
        }
        profile.m_entries.append(entry);
    }
    profile.m_trailing = formatting;
    return profile;
}

QString Openvpn3Profile::render(const Openvpn3Entry &entry, const QString &newline)
{
    // A changed entry keeps what was around it: the terminator its line had
    // -- none, for a final line without one -- and the comment at its end.
    const QString terminator = entry.m_parsed ? entry.m_terminator : newline;
    switch (entry.kind) {
    case Openvpn3Entry::Block: {
        QString text = u'<' + entry.name + u'>' + entry.m_comment + newline;
        text += entry.body;
        if (!entry.body.isEmpty() && !entry.body.endsWith(u'\n')) {
            text += newline;
        }
        text += QLatin1String("</") + entry.name + u'>' + terminator;
        return text;
    }
    case Openvpn3Entry::Directive:
        break;
    }

    QString text = entry.name;
    for (const QString &argument : entry.arguments) {
        text += u' ' + quoteArgument(argument);
    }
    // A comment that was glued to the last word is still a comment for
    // openvpn3, but a space in front of it makes it one for everybody.
    if (!entry.m_comment.isEmpty() && !isAsciiSpace(entry.m_comment.front())) {
        text += u' ';
    }
    return text + entry.m_comment + terminator;
}

QString Openvpn3Profile::newline() const
{
    // The convention of the document is that of its first terminated line,
    // whatever carries it.
    const QString text = [this] {
        QString all;
        for (const Openvpn3Entry &entry : m_entries) {
            all += entry.m_leading;
            if (entry.m_parsed) {
                all += entry.m_source;
            }
            if (all.contains(u'\n')) {
                break;
            }
        }
        return all + m_trailing;
    }();
    const int end = text.indexOf(u'\n');
    return end > 0 && text.at(end - 1) == u'\r' ? QStringLiteral("\r\n") : QStringLiteral("\n");
}

QString Openvpn3Profile::textOf(const Openvpn3Entry &entry, const QString &newline) const
{
    return entry.m_leading + (entry.m_verbatim ? entry.m_source : render(entry, newline));
}

QString Openvpn3Profile::toText() const
{
    const QString nl = newline();
    QString text;
    const auto add = [&text, &nl](const QString &piece) {
        if (piece.isEmpty()) {
            return;
        }
        // A formerly final, unterminated line may now have a successor. Keep
        // its source intact, but separate it using the document's convention.
        if (!text.isEmpty() && !text.endsWith(u'\n')) {
            text += nl;
        }
        text += piece;
    };
    for (const Openvpn3Entry &entry : m_entries) {
        add(textOf(entry, nl));
    }
    add(m_trailing);
    return text;
}

QString Openvpn3Profile::optionsText() const
{
    const QString nl = newline();
    QString text;
    for (const Openvpn3Entry &original : m_entries) {
        Openvpn3Entry entry = original;
        QString piece;
        if (!entry.m_verbatim) {
            entry.m_comment.clear();
            if (entry.isBlock() && isOptionScope(entry.name)) {
                entry.body = fromText(entry.body).optionsText();
            }
            piece = render(entry, nl);
        } else {
            // The opening line without its comment, then the rest.
            const int end = entry.m_source.indexOf(u'\n');
            const QString first = end < 0 ? entry.m_source : entry.m_source.left(end + 1);
            const auto [content, terminator] = splitTerminator(first);
            piece = content.chopped(entry.m_comment.size()) + terminator;
            QString rest = entry.m_source.mid(first.size());
            if (entry.isBlock() && isOptionScope(entry.name) && rest.size() > entry.body.size()) {
                // A scope's lines are options; its formatting goes too -- but
                // not that of an unterminated one, which openvpn3 reads as
                // no scope at all and which is shown as it stands.
                rest = fromText(entry.body).optionsText() + rest.mid(entry.body.size());
            }
            piece += rest;
        }
        if (!text.isEmpty() && !text.endsWith(u'\n')) {
            text += nl;
        }
        text += piece;
    }
    return text;
}

QList<int> Openvpn3Profile::indexesOf(const QString &name) const
{
    QList<int> indexes;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).name == name) {
            indexes.append(i);
        }
    }
    return indexes;
}

int Openvpn3Profile::indexOf(const QString &name) const
{
    const QList<int> indexes = indexesOf(name);
    return indexes.isEmpty() ? -1 : indexes.first();
}

int Openvpn3Profile::indexOfId(quint64 id) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).id() == id) {
            return i;
        }
    }
    return -1;
}

QList<Openvpn3Entry> Openvpn3Profile::optionEntries() const
{
    QList<Openvpn3Entry> options;
    for (const Openvpn3Entry &entry : m_entries) {
        if (entry.isBlock() && isOptionScope(entry.name)) {
            // Its lines are options in their own right; the parser reads them
            // the same way it read the document, so a block nested in here
            // stays one entry and its payload stays inside it.
            options += fromText(entry.body).optionEntries();
            continue;
        }
        options.append(entry);
    }
    return options;
}

QList<Openvpn3Entry> Openvpn3Profile::optionsNamed(const QString &name) const
{
    QList<Openvpn3Entry> found;
    const QList<Openvpn3Entry> options = optionEntries();
    for (const Openvpn3Entry &entry : options) {
        if (entry.name == name) {
            found.append(entry);
        }
    }
    return found;
}

QStringList Openvpn3Profile::remoteHosts() const
{
    QStringList hosts;
    const QList<Openvpn3Entry> remotes = optionsNamed(QStringLiteral("remote"));
    for (const Openvpn3Entry &entry : remotes) {
        hosts.append(entry.value());
    }
    return hosts;
}

bool Openvpn3Profile::needsNormalization() const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (needsNormalizationAt(i)) {
            return true;
        }
    }
    return false;
}

bool Openvpn3Profile::needsNormalizationAt(int index) const
{
    const Openvpn3Entry &entry = m_entries.at(index);
    if (entry.isBlock()) {
        // Credentials typed into the editor: the username and password
        // belong with the connection, not in the profile.
        return entry.name == QLatin1String("auth-user-pass");
    }
    if (entry.arguments.isEmpty()) {
        return false;
    }
    if (entry.name == QLatin1String("auth-user-pass")) {
        return true; // a credentials file
    }
    if (!isFileDirective(entry.name) || entry.value() == QLatin1String("[inline]")) {
        return false;
    }
    // A directory openvpn reads itself, not a file to inline.
    return !(entry.name == QLatin1String("crl-verify") && entry.arguments.value(1) == QLatin1String("dir"));
}

QString Openvpn3Profile::importerTagDisagreement() const
{
    // What ovpn-import.c compares: the line with ASCII whitespace stripped
    // from both ends (g_strstrip), comment and quotes and all.
    const auto stripped = [](const QString &line) {
        QString text = splitTerminator(line).first;
        while (!text.isEmpty() && isAsciiSpace(text.front())) {
            text.remove(0, 1);
        }
        while (!text.isEmpty() && isAsciiSpace(text.back())) {
            text.chop(1);
        }
        return text;
    };
    for (const Openvpn3Entry &entry : m_entries) {
        const QString first = stripped(entry.m_source.left(entry.m_source.indexOf(u'\n') + 1));
        if (entry.isDirective()) {
            // A line the importer takes for an opening tag and openvpn3 does
            // not: what follows would be payload to one and directives to
            // the other.
            if (isOpeningTag(first)) {
                return first.mid(1, first.size() - 2);
            }
            continue;
        }
        if (!entry.m_parsed) {
            continue; // rendered here, so canonical
        }
        if (first != u'<' + entry.name + u'>') {
            return entry.name;
        }
        // The importer would close the block early on a padded closing tag.
        const QString closing = QLatin1String("</") + entry.name + u'>';
        for (const QString &line : linesWithTerminators(entry.body)) {
            if (stripped(line) == closing) {
                return entry.name;
            }
        }
    }
    return QString();
}

int Openvpn3Profile::connectionBlockCost(const QString &body)
{
    // What OptionList::parse_from_config() charges a block with when it is
    // given Limits: the text, every line counted with the '\n' openvpn3 joins
    // it with, then OPT_OVERHEAD (64) per option and TERM_OVERHEAD (16) per
    // word of every option line it lexes. A nested block's payload is text
    // and nothing more; comment and blank lines are text too.
    int cost = 0;
    const QStringList lines = linesWithTerminators(body);
    QString nested;
    for (const QString &raw : lines) {
        const QString line = splitTerminator(raw).first;
        cost += line.toUtf8().size() + 1;
        const QString stripped = line.trimmed();
        if (!nested.isEmpty()) {
            if (line == nested) {
                nested.clear();
                cost += 64;
            }
            continue;
        }
        if (isBlankLine(raw) || stripped.startsWith(u'#') || stripped.startsWith(u';')) {
            continue;
        }
        const QStringList words = splitArguments(line);
        if (words.isEmpty()) {
            continue;
        }
        cost += 16 * words.size();
        if (words.size() == 1 && isOpeningTag(words.first())) {
            nested = QLatin1String("</") + words.first().mid(1);
        } else {
            cost += 64;
        }
    }
    return cost;
}

QList<Openvpn3Profile::ConnectionCaveat> Openvpn3Profile::connectionCaveats() const
{
    QList<ConnectionCaveat> caveats;
    int block = 0;
    for (const Openvpn3Entry &entry : m_entries) {
        if (!entry.isBlock() || !isOptionScope(entry.name)) {
            continue;
        }
        ++block;
        const Openvpn3Profile scope = fromText(entry.body);
        for (const Openvpn3Entry &option : scope.m_entries) {
            if (option.name == QLatin1String("remote") || option.name == QLatin1String("proto") || option.name == QLatin1String("port")) {
                continue;
            }
            if (option.isDirective() && isFileDirective(option.name) && !option.arguments.isEmpty() && option.value() != QLatin1String("[inline]")) {
                caveats.append({ConnectionCaveat::FileReference, block, option.name, option.value()});
            } else {
                caveats.append({ConnectionCaveat::IgnoredOption, block, option.name, QString()});
            }
        }
        const int cost = connectionBlockCost(entry.body);
        if (cost >= kMaxConnectionBlockCost) {
            caveats.append({ConnectionCaveat::TooLarge, block, QString(), QString::number(cost)});
        }
    }
    return caveats;
}

bool Openvpn3Profile::usesCrlf() const
{
    int crlf = 0;
    int lf = 0;
    const QString text = toText();
    for (int i = text.indexOf(u'\n'); i >= 0; i = text.indexOf(u'\n', i + 1)) {
        (i > 0 && text.at(i - 1) == u'\r') ? ++crlf : ++lf;
    }
    return crlf > lf;
}

QString Openvpn3Profile::value(const QString &name) const
{
    return arguments(name).value(0);
}

QStringList Openvpn3Profile::arguments(const QString &name) const
{
    const int index = indexOf(name);
    return index < 0 ? QStringList() : m_entries.at(index).arguments;
}

QString Openvpn3Profile::blockBody(const QString &name) const
{
    for (const Openvpn3Entry &entry : m_entries) {
        if (entry.isBlock() && entry.name == name) {
            return entry.body;
        }
    }
    return QString();
}

QString Openvpn3Profile::sourceAt(int index) const
{
    const Openvpn3Entry &entry = m_entries.at(index);
    return entry.m_verbatim ? entry.m_source : render(entry, newline());
}

bool Openvpn3Profile::mayNeedPrivateKeyPassphrase() const
{
    // A <connection> block's included: openvpn3 itself takes nothing but the
    // remote from one, but the backend's service looks for key material
    // anywhere in the profile and asks for its passphrase.
    for (const Openvpn3Entry &entry : optionEntries()) {
        if (entry.name != QLatin1String("key") && entry.name != QLatin1String("pkcs12")) {
            continue;
        }
        if (!entry.isBlock()) {
            // A file reference: nothing here can look inside it.
            return !entry.arguments.isEmpty();
        }
        if (entry.name == QLatin1String("pkcs12")) {
            // Whether a bundle is encrypted only shows on opening it, which
            // is the service's job, not the editor's.
            return true;
        }
        // The two ways OpenSSL marks an encrypted private key.
        if (entry.body.contains(QLatin1String("ENCRYPTED PRIVATE KEY")) || entry.body.contains(QLatin1String("Proc-Type: 4,ENCRYPTED"))) {
            return true;
        }
    }
    return false;
}

void Openvpn3Profile::setArguments(int index, const QStringList &arguments)
{
    Openvpn3Entry &entry = m_entries[index];
    if (entry.arguments == arguments) {
        return; // unchanged: keep the source, and with it the original quoting
    }
    entry.arguments = arguments;
    entry.m_verbatim = false;
}

void Openvpn3Profile::setBody(int index, const QString &body)
{
    Openvpn3Entry &entry = m_entries[index];
    // A body handed back by a text box has lost its carriage returns; in a
    // document that has them, it gets them back rather than mixing the two.
    const QString kept = entry.body.contains(QLatin1String("\r\n")) || (!entry.m_parsed && newline() != QLatin1String("\n"))
        ? withNewlines(body, QStringLiteral("\r\n"))
        : body;
    if (entry.body == kept) {
        return;
    }
    entry.body = kept;
    entry.m_verbatim = false;
}

void Openvpn3Profile::rename(int index, const QString &name)
{
    Openvpn3Entry &entry = m_entries[index];
    if (entry.name == name) {
        return;
    }
    entry.name = name;
    entry.m_verbatim = false;
}

void Openvpn3Profile::replace(int index, const Openvpn3Entry &entry)
{
    m_entries[index] = entry;
}

void Openvpn3Profile::replaceWith(int index, const QList<Openvpn3Entry> &entries)
{
    if (entries.isEmpty()) {
        removeAt(index);
        return;
    }
    const Openvpn3Entry old = m_entries.at(index);
    QList<Openvpn3Entry> added = entries;
    QString leading = old.m_leading;
    const QString comment = old.m_comment.trimmed();
    if (!comment.isEmpty()) {
        // The comment said something about the line it was on; on a line of
        // its own above what replaced it, it still does.
        leading += comment + (old.m_parsed && !old.m_terminator.isEmpty() ? old.m_terminator : newline());
    }
    added.first().m_leading = leading + added.first().m_leading;
    // The last of them ends the way the old entry's line did, so a document
    // without a final newline still has none.
    Openvpn3Entry &last = added.last();
    if (old.m_parsed && old.m_terminator.isEmpty() && last.m_parsed && !last.m_terminator.isEmpty()) {
        if (last.m_verbatim) {
            last.m_source.chop(last.m_terminator.size());
        }
        last.m_terminator.clear();
    }
    m_entries.removeAt(index);
    for (int i = 0; i < added.size(); ++i) {
        m_entries.insert(index + i, added.at(i));
    }
}

void Openvpn3Profile::removeAt(int index)
{
    // The formatting in front of an entry is not the entry: it stays where it
    // is, in front of whatever comes next. So does what the comment at the end
    // of its line said, on a line of its own where the line was.
    const Openvpn3Entry old = m_entries.at(index);
    const QString comment = old.m_comment.trimmed();
    const QString kept = old.m_leading + (comment.isEmpty() ? QString() : comment + (old.m_parsed ? old.m_terminator : newline()));
    m_entries.removeAt(index);
    if (kept.isEmpty()) {
        return;
    }
    if (index < m_entries.size()) {
        m_entries[index].m_leading.prepend(kept);
    } else {
        // At the end, a line without a terminator is followed by nothing; give
        // whatever was already trailing a line of its own.
        if (!m_trailing.isEmpty() && !kept.endsWith(u'\n')) {
            m_trailing.prepend(newline());
        }
        m_trailing.prepend(kept);
    }
}

void Openvpn3Profile::removeAll(const QString &name)
{
    const QList<int> indexes = indexesOf(name);
    for (auto it = indexes.crbegin(); it != indexes.crend(); ++it) {
        removeAt(*it);
    }
}

int Openvpn3Profile::append(const Openvpn3Entry &entry)
{
    m_entries.append(entry);
    return m_entries.size() - 1;
}

void Openvpn3Profile::insert(int index, const Openvpn3Entry &entry)
{
    m_entries.insert(index, entry);
}

void Openvpn3Profile::move(int from, int to)
{
    if (from == to) {
        return;
    }
    // A move is a rotation of the entries between the two places.
    const int low = qMin(from, to);
    const int high = qMax(from, to);
    QList<int> places;
    QList<quint64> ids;
    for (int i = low; i <= high; ++i) {
        places.append(i);
        ids.append(m_entries.at(i).id());
    }
    if (from < to) {
        ids.append(ids.takeFirst());
    } else {
        ids.prepend(ids.takeLast());
    }
    reorder(places, ids);
}

void Openvpn3Profile::reorder(const QList<int> &places, const QList<quint64> &ids)
{
    if (places.size() != ids.size()) {
        return;
    }
    QList<Openvpn3Entry> moving;
    QStringList detached;
    for (int i = 0; i < places.size(); ++i) {
        const int index = indexOfId(ids.at(i));
        if (index < 0) {
            return;
        }
        moving.append(m_entries.at(index));
        detached.append(splitFormatting(m_entries.at(places.at(i)).m_leading).first);
    }
    for (int i = 0; i < places.size(); ++i) {
        Openvpn3Entry entry = moving.at(i);
        entry.m_leading = detached.at(i) + splitFormatting(entry.m_leading).second;
        m_entries[places.at(i)] = entry;
    }
}

void Openvpn3Profile::setDirective(const QString &name, const QStringList &arguments)
{
    const int index = indexOf(name);
    if (index < 0) {
        append(directive(name, arguments));
    } else {
        setArguments(index, arguments);
    }
}

void Openvpn3Profile::setPresent(const QString &name, bool present)
{
    const int index = indexOf(name);
    if (present && index < 0) {
        append(directive(name));
    } else if (!present && index >= 0) {
        removeAll(name);
    }
}

void Openvpn3Profile::setBlock(const QString &name, const QString &body)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).isBlock() && m_entries.at(i).name == name) {
            setBody(i, body);
            return;
        }
    }
    append(block(name, body));
}

Openvpn3Entry Openvpn3Profile::directive(const QString &name, const QStringList &arguments)
{
    Openvpn3Entry entry;
    entry.kind = Openvpn3Entry::Directive;
    entry.name = name;
    entry.arguments = arguments;
    return entry;
}

Openvpn3Entry Openvpn3Profile::block(const QString &name, const QString &body)
{
    Openvpn3Entry entry;
    entry.kind = Openvpn3Entry::Block;
    entry.name = name;
    entry.body = body;
    return entry;
}
