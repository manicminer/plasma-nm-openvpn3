# Limitations

What this module does not do, and why. Everything here is deliberate and
tested; nothing here is a surprise waiting to be found.

## What a stock plasma-nm host does not offer

### There is no OpenVPN 3 entry in the top-level "Import VPN connection"

This is deliberate, and it is the one thing this module gives up in order to
leave an unmodified host unmodified.

A stock plasma-nm's top-level import asks every installed VPN plugin whether it
reads the file's extension and uses the first one that says yes, in whatever
order the plugins happened to be found. Both OpenVPN plugins read `.ovpn`, and
the host has no way to ask which was meant. If this module claimed `.ovpn`,
then installing it would sometimes turn somebody's OpenVPN 2 import into an
OpenVPN 3 connection — a change to how an unrelated VPN behaves, made silently
on a machine where nothing else was asked for. So it claims nothing, and
OpenVPN 2 keeps every `.ovpn` and `.conf`.

Importing a profile as an OpenVPN 3 connection is still two clicks: add an
OpenVPN 3 connection, then use **Import Profile…** on its editor page. That
goes straight to the backend's own importer and never touches the host's
chooser. It is also the only path that can ask where the profile should be
kept, which the top-level import never could.

`host-patches/0003-ask-which-vpn-plugin-should-import-a-shared-extension.patch`
is what a host that could ask the question would look like. It is not applied
by this build and is in no package here.

### A connection with no `profile-flags` may open without its profile

A connection editor asks NetworkManager for the secrets a connection records
that it keeps, and that record is the `*-flags` entries in its VPN data.
Everything this module writes records `profile-flags`, so its own connections
are never affected.

A secret-mode connection written by something else may omit it. NetworkManager
reads an absent entry as system-owned and the profile is still there, but the
editor may never be given it, which looks exactly like a locked wallet from
inside the editor. It therefore fails closed — saving is blocked so the stored
profile cannot be lost — and says which entry is missing and the `nmcli`
command that adds it.

The host asks for *all* of a connection's VPN secrets when *any* one of its
`*-flags` entries says a secret is kept, so a connection that records, say,
`password-flags` gets its profile anyway. That case is locked for some other
reason and does not get the explanation, because the entry it names would not
be the problem.

`host-patches/0001-offer-an-ipv6-page-and-request-secrets-for-openvpn3.patch`
makes the host ask for such a profile anyway.

### An OpenVPN 3 connection gets no IPv6 page

Stock plasma-nm decides which connections get an IPv6 page inside its own
connection editor, by connection type and — for a VPN — by service name, and
the only VPN service on that list is OpenVPN 2's. An OpenVPN 3 connection
therefore gets IPv4 and no IPv6, so its IPv6 settings are neither shown nor
editable here and the editor writes back whatever the connection type
defaults to. Configure them with `nmcli` or another editor if you need
something other than the default.

No plugin can add itself to that list. A setting widget's map is inserted
under its own type, so this module's page can only ever contribute the `vpn`
setting; the page list, the widget list and the decision are all private to
`ConnectionEditorBase`. Reaching into the host's widgets by name to bolt a
page on was considered and rejected: it would depend on the internal layout of
a library with no stable ABI and break in a way nobody could diagnose.

`host-patches/0001-offer-an-ipv6-page-and-request-secrets-for-openvpn3.patch`
adds the service to that list.

### The editor drops connection properties it does not show

Saving a connection through plasma-nm's connection editor loses the
`connection.*` properties that nothing in the editor shows. Its General page
builds a fresh connection setting out of its own controls, so a property that
is on no page, and that the editor does not go out of its way to put back
afterwards, is not in what gets saved. On an OpenVPN 3 connection that
includes `interface-name`, `mdns`, `llmnr`, `autoconnect-retries`,
`gateway-ping-timeout`, `lldp`, `stable-id`, `auth-retries`, `mud-url` and
`wait-device-timeout`; the test below pins four of them.

Some unshown properties are put back by hand and do survive: the `uuid`, the
`master` and `slave-type` that make a connection a member of a bond or bridge,
and `interface-name` for a WireGuard connection, which comes from WireGuard's
own page.

What the General page itself shows and keeps is the host's business and is not
documented here; this is only about what it does not show.

**This is not caused by this module and is not specific to OpenVPN 3.** It
happens on a stock plasma-nm with or without this installed, to every
connection type that has no page of its own setting the property in question.
It is listed here because an OpenVPN 3 connection is subject to it like any
other, and because it cannot be fixed from a plugin for the same reason the
IPv6 page cannot.

`host-patches/0002-preserve-connection-properties-the-editor-does-not-show.patch`
fixes it in the host, for every connection type.

### These are tested, not asserted

`autotests/openvpn3hostcompattest.cpp` subclasses the installed plasma-nm's
own connection editor and measures what it actually does with an OpenVPN 3
connection: which pages it offers, and which properties survive a save. A
plasma-nm that fixes one of these makes an assertion there fail, so this page
gets corrected rather than quietly going stale.

Its third assertion is weaker and says so: that what this module writes
satisfies the condition under which the host asks NetworkManager for a
connection's secrets. That condition is reimplemented in this project and
pinned to plasma-nm 6.7.5, because a real `GetSecrets` round trip needs a
NetworkManager to make it to and these are component tests with no bus. It
asserts the contract, not the conversation.

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
