# Limitations

What this module does not do, and why. Everything here is deliberate and
tested; nothing here is a surprise waiting to be found.

## Profile parsing

### A backslash inside single quotes follows OpenVPN 2, not openvpn3

The two OpenVPN lexers disagree about one thing, and there is no reading that
satisfies both. OpenVPN 2's `parse_line()` turns its escape handling off inside
a single-quoted parameter (`STATE_READING_SQUOTED_PARM`), so a backslash there
is a literal character and the apostrophe after it closes the quote. openvpn3's
`StandardLex` lets the backslash escape that apostrophe and stays inside the
quote. For `setenv a 'x\' # literal'` OpenVPN 2 reads the value `x\`, and
openvpn3 reads `x' # literal`.

This module reads it as OpenVPN 2 does. openvpn3 is what actually reads the
stored profile, so where it matters is this:

* **Reading such a line never damages it.** Comment detection treats a `#` or
  `;` as a comment only where *both* lexers agree it is one, so nothing is cut
  off a line like that, and an entry nobody edited is written back from its
  original text, byte for byte.
* **Editing such a line rewrites it as the table read it.** If you change
  another argument on that line, the single-quoted value is re-rendered from
  the OpenVPN 2 reading, and what openvpn3 sees afterwards changes.

A single-quoted argument containing a backslash is the whole of the exposure.
Use double quotes, where the two lexers agree, if you need an escape.

Everywhere else a backslash is an escape, as both lexers have it: outside
quotes and inside double quotes it takes the character after it, so an escaped
space is part of the value in front of it rather than the end of it, including
at the end of a line, where there is nothing after it to say so. A backslash
with nothing after it at all escapes nothing and stays the character it is.

### Formatting is not preserved

Comments and blank lines are discarded when a profile is read. openvpn3 reads
nothing from them, and the profile stored inside a NetworkManager connection
is not a file anybody opens in an editor again, so keeping them would only add
rows to the directives table that nothing can act on. The backend's own
importer does the same, so an imported profile arrives without them and a
reimport changes nothing further.

What is *not* formatting is kept: the lines of an opaque `<ca>`, `<key>`,
`<auth-user-pass>` or unknown `<tag>` payload are content and are untouched,
blank lines and empty credentials included; a line of `U+00A0` or of `\v` is a
value, not a blank line; and the whitespace a directive line is padded with
belongs to that line.

### Exporting a profile is refused

An OpenVPN 3 profile is self-contained: it has the private key and the
certificates inlined in it, and when it is kept with the connection's secrets
that is precisely so it never reaches a plain file. There is no export, rather
than an export that quietly leaks one.
