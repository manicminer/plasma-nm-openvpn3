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

### A marker-only connection with no `profile-flags` may open without its profile

A connection editor asks NetworkManager for the secrets a connection records
that it keeps, and that record is the `*-flags` entries in its VPN data.
Everything this module and the upstream backend write records `profile-flags`
-- in the backend's format its presence is what makes the profile a secret --
so their connections are never affected.

A connection in the older layout, `profile-storage=secret`, written by
something else may omit it. NetworkManager
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
pinned to plasma-nm 6.8.2, because a real `GetSecrets` round trip needs a
NetworkManager to make it to and these are component tests with no bus. It
asserts the contract, not the conversation.

## Profile parsing

### Values are read the way openvpn3 reads them

openvpn3 is what reads the stored profile, so the table, the General page and
every edit read a line with openvpn3's own lexer (`OptionList::LexComment`
through `Split::by_space`, as of the openvpn3 sources this was checked
against): a `#` or `;` outside quotes starts a comment anywhere, glued to a
word too (`remote vpn.example.org#primary` is the host `vpn.example.org`); a
backslash escapes the next character everywhere, inside single quotes
included; and an escaped space still separates.

OpenVPN 2 reads some of those lines differently — for it `a#b` is a value and
a backslash inside single quotes is literal. That does not change anything
here:

* **A line nobody edited is written back exactly as it was**, so its meaning
  to either is untouched.
* **Editing a line rewrites it from what openvpn3 read.** The other
  arguments come back quoted where they need it, so openvpn3 reads the same
  values from them; a comment glued to the line is kept, with a space in
  front of it.

### Comments and blank lines are kept, and not shown in the table

The profile is one document, and the editor keeps all of it: comment lines,
blank lines, a comment at the end of a directive, the formatting inside a
`<connection>` block, CRLF line endings and a missing final newline. The
**Profile Source** page shows the stored document exactly as it is stored —
opening the page or switching tabs never reimports or rewrites it. (The text
box itself has no carriage returns to show; a CRLF document stays CRLF.)

The **Directives** table lists directives and blocks only; formatting has no
row. It is still in the document, and the table's edits treat it this way:

* **Editing** a directive rewrites that one line — its quoting and padding
  become the editor's — and keeps the comment at its end, the lines in front
  of it and its line ending. Nothing else changes.
* **Adding** a directive puts it after the selected row, in front of the
  comments of the row after it.
* **Removing** a directive removes it. Comment and blank lines in front of
  it stay in the document, in front of whatever follows, and a comment at the
  end of its line stays as a comment line of its own where the line was.
* **Moving** a directive takes the comment lines directly above it along;
  a blank line, and anything above one, stays where it was. Reordering
  servers on the General page does the same.

Comments are edited on the Profile Source page; the table has no button for
them.

Where a block ends is decided as openvpn3 decides it: a block opens on a
line whose one word is a tag, quoted or padded, and closes only on a line
that is exactly its closing tag. An indented closing tag, or one with a
comment after it, is still payload, and nothing after it is read as a
directive until the real one.

An imported profile is what the backend's importer made of the file, which
keeps comments and blank lines but strips each line's surrounding whitespace;
see [backend.md](backend.md#what-the-import-does-to-a-profile-and-what-it-does-not).
The editor promises the stored document, not the original file's bytes.

### File references and the OpenVPN 3 backend

openvpn3 is given the profile and nothing else, so a profile has to be
self-contained when it is saved.

* **Typed or edited references** at the top level — `ca`, `cert`, `key`,
  `extra-certs`, `tls-auth`, `tls-crypt`, `tls-crypt-v2`, `crl-verify` with a
  file, `pkcs12`, `auth-user-pass` with a file, an inline `<auth-user-pass>` —
  are resolved through the backend's own importer when the page is left or
  the connection saved. A stored profile that still has such entries —
  written by something else — is shown as stored however often the pages
  are visited, and is made self-contained only when it is saved. (Once
  credentials are typed in, the stored credential sources are resolved with
  them, in order, as the backend would.) Only those entries go through it, one at a time, and
  what it makes of each is put in its place; the rest of the document is not
  touched, so the importer's whitespace normalization never reaches lines
  nobody edited. A comment on a replaced line is kept on a line of its own.
* **A relative path is refused** with an explanation: a document held in the
  editor has no folder it could be relative to. Use the full path, or import
  the `.ovpn` file, whose own folder is used.
* **A file that cannot be read** blocks saving, naming the path; nothing in
  the document is changed meanwhile, and the file is looked for again each
  time, so putting it in place is enough.
* **Inside a `<connection>` block nothing is resolved**, neither by the
  backend's importer nor by this editor, and nothing is moved out of the
  block: openvpn3's `RemoteList` reads only `remote`, `proto` and `port` from
  a block, so a `ca` or `auth-user-pass` there is something openvpn3 ignores,
  not something to hoist. The editor shows a caveat naming each option
  openvpn3 ignores and each file left unembedded, and saves the block as
  written. An `auth-user-pass` or encrypted key inside a block is the one
  exception that still counts: openvpn3 ignores it, but the backend's service
  (`ovpn.needs_user_pass()`, `pki.py`) does not — it refuses to connect
  without a username and asks for the password or passphrase — so the editor
  asks for them too. The backend's C importer sets no `password-flags` for
  such a profile; saving it here records them.
* **openvpn3 refuses a `<connection>` block** whose size, as it counts it —
  the text, comments included, plus 64 bytes per option and 16 per argument
  — reaches 4096. The editor estimates that count and warns; the estimate
  follows openvpn3's `OptionList::Limits` but is not openvpn3 itself.
* **`crl-verify <dir> dir`** stays a directory reference. Whether openvpn3
  uses a CRL directory at run time is not established here.
* **Not embedded by the backend at all:** a legacy `http-proxy host port
  authfile` credentials file (openvpn3 accepts inline `<http-proxy-user-pass>`
  instead), and anything a `config` directive would include. Script, plugin,
  `askpass` and `capath` references are not embedded either; openvpn3 does
  not run those as OpenVPN 2 does.

### Exporting a profile is refused

An OpenVPN 3 profile is self-contained: it has the private key and the
certificates inlined in it, and when it is kept with the connection's secrets
that is precisely so it never reaches a plain file. There is no export from
Plasma, rather than an export that quietly leaks one.

The backend's own `nmcli connection export type openvpn3` does write the
profile out, private key included, to a file it makes 0600. That is the
backend's choice for its command-line path and does not change this one.

## What has not been shown

Everything in this repository is a component test. They are run against the
distribution's own plasma-nm, against the backend's own libnm importer, and
against the module as it is actually installed — but there are things they do
not reach, and it is worth saying which.

* **No run has combined this editor, a wallet and a live NetworkManager.**
  The tests have no session bus with a real secret store on it and no
  NetworkManager to talk to. Wallet availability is checked against real
  isolated buses with and without a provider, and the providers there are
  synthetic. Saving to a wallet, activating the connection and getting traffic
  has not been done as one sequence anywhere.
* **No live `GetSecrets` round trip.** The condition under which the host asks
  NetworkManager for a connection's secrets is reimplemented here and compared
  against what this module writes; the request itself is not made.
* **The KCM's save error propagation is not covered end to end.** These tests
  drive the editor page and the host's connection editor, not the whole
  configuration module.
* **No live activation.** No run in this repository has brought up a
  connection through a real NetworkManager and openvpn3. The backend's
  conformance suite does take the connections this editor saves through the
  backend's real service code against its fake openvpn3, and they connect
  there; that is not a tunnel.
* **Comment preservation is shown for this editor's documents, not for
  every exotic profile.** The parser keeps every byte it does not change;
  what a profile means where OpenVPN 2 and openvpn3 disagree is described
  above.
* **A separate real-KWallet probe, run elsewhere, did not pass cleanly.** It
  verified an 8,172-byte synthetic secret map through write, independent read
  and a daemon restart, and its cancellation checks *failed*: cancelling an
  unlock — including after a wrong password — returned QtKeychain `NoError`
  with empty data. That suite finished 56 assertions passed, 2 failed, not
  all-green. This module treats an empty or missing profile reply as
  unavailable, keeps the original map, and cannot save an empty replacement,
  which is what makes that behaviour survivable rather than fixed. The probe
  is not part of this repository.
