# The backend this needs

This module is the Plasma editor. The thing that actually connects is the
NetworkManager OpenVPN 3 backend, which provides the
`org.freedesktop.NetworkManager.openvpn3` service, its
`nm-openvpn3-service.name` file and the libnm editor plugin this module's
importer calls:

<https://github.com/AlexeySetevoi/network-manager-openvpn3>

## The short version

**Upstream `network-manager-openvpn3` 0.2.0 or later.** No fork, no
unmerged patch. Tested against upstream
`6f9dd3d527ff3e3ae5a6098df1be675f93ce34c5` (0.2.0 plus its test-layout
follow-up); CI builds against exactly that revision.

Against 0.1.0 a connection this editor saves will not activate: 0.1.0 reads
the profile only from a public data item, and this editor keeps every new
profile as a secret.

## The connection format

The backend's [`docs/connection-format.md`](https://github.com/AlexeySetevoi/network-manager-openvpn3/blob/main/docs/connection-format.md)
is the contract, and this editor writes it:

| | `vpn.data` | `vpn.secrets` |
| --- | --- | --- |
| What this editor writes | `profile-flags` = `1` (wallet) or `0` (all users), `remote` = first remote host, for display | `profile` = base64 of the profile text |
| Read, never written | `profile-storage` = `secret`, the marker an earlier version of this editor wrote, with `profile-flags`, or without it meaning `0` | the same |
| Read, kept if it is what the connection uses | `profile` = base64 profile, no `profile-flags` (the layout before 0.2.0) | — |

* **The presence of `profile-flags` makes the profile a secret**, as the
  backend reads it. Only `0` and `1` are accepted; anything else — `2`, `4`,
  an empty value, not a number — means the connection is broken and the
  editor refuses to save it rather than guess.
* **A `vpn.data` copy next to the flags is stale and is never read**, by the
  editor, its secrets prompt or the backend. A locked wallet is reported as a
  locked wallet, not answered from the leftover copy.
* **A `profile-storage` value other than `secret` is refused**, flags or not.
* **Saving a secret profile writes the backend's layout exactly**: the flags,
  the secret and `remote`; the `profile-storage` marker and any public copy
  are removed.
* **Wallet or all users is kept as it was.** A wallet connection stays in the
  wallet and an unattended one stays system-owned; a connection whose older
  marker had no flags is saved as system-owned (`0`), which is how
  NetworkManager already read it.
* **The public layout is kept for a connection that already uses it**, and
  offered only to such a connection, labelled as unprotected. The backend
  still reads it (its package upgrade moves connections on its own); this
  editor does not move anybody's keys as a side effect of opening the page.
  Choosing the wallet or all-users storage on the editor page moves it.
* The whole profile remains one secret, one-time codes are never stored, and
  a secrets reply that arrives after the profile was edited cannot replace
  the edit.
* **The secrets prompt answers nothing for a layout it cannot vouch for** —
  an unknown marker or unusable flags — not even on a hinted retry: plasma-nm
  takes the prompt's answer whatever it says about itself, so the refusal is
  an empty answer, and the activation fails for want of a profile.

## Importing

Both import paths go through the backend's own libnm importer
(`nm_vpn_editor_plugin_import()`), so what is stored is what the backend
would store:

* **Import Profile… on the editor page** imports with the storage chosen on
  the page — the wallet by default — and keeps the connection's identity,
  addresses and routes.
* **The module's plugin import** (`VpnUiPlugin::importConnectionSettings`,
  which a stock plasma-nm only reaches through code, see
  [limitations.md](limitations.md)) imports into the wallet, and refuses when
  there is none.

The backend itself imports a profile as a system-owned secret. This editor
keeps preferring the wallet for a new connection, asking first; choosing
storage for all users gives exactly the backend's layout.

### What the import does to a profile, and what it does not

From the backend's `libnm-plugin/ovpn-import.c` at the tested revision, and
pinned by this repository's importer tests against the real plugin:

* **Inlined, at the top level only:** `ca`, `cert`, `key`, `extra-certs`,
  `tls-auth` (its direction becomes `key-direction`), `tls-crypt`,
  `tls-crypt-v2`, `crl-verify` with a file. `pkcs12` becomes a base64
  `<pkcs12>` block; the service converts it when connecting. Relative paths
  are read relative to the profile's own directory, quoted paths with spaces
  included. A file that cannot be read fails the import, naming the path.
* **Credentials:** an inline `<auth-user-pass>` or the file `auth-user-pass`
  names moves to the connection's username and password, and one bare
  `auth-user-pass` stays so that openvpn3 asks.
* **Not inlined:** `crl-verify <dir> dir` stays a directory reference.
  Nothing inside a `<connection>` block is inlined or lifted out — the block
  is kept as written, a file reference or credentials in it included. There
  are no nested `config` includes, no `cd`-relative resolution, and no
  embedding of script, plugin, `askpass`, `capath` or proxy credential files.
* **Kept:** comments and blank lines, directive order, repetitions, unknown
  options, opaque `<tag>` blocks.
* **Refused here:** a profile whose block tags the importer reads
  differently from openvpn3 — a quoted opening tag, a comment after one, an
  indented closing tag. The importer matches the literal stripped line, so
  it would read what openvpn3 holds as a `<connection>` scope or a payload as
  directives, and inline the files or lift the credentials in it. Both import
  paths refuse such a file and say to put each tag on a line of its own.
* **Normalized:** every line outside a payload is stripped of leading and
  trailing whitespace — an escaped trailing space included — and a payload
  line loses its trailing whitespace; a comment after a reference that is
  inlined is lost with it. The stored profile is the backend's
  normalization of the file, not the file's exact bytes.

See [limitations.md](limitations.md#file-references-and-the-openvpn-3-backend)
for what this means in use, and what openvpn3 itself does with a
`<connection>` block.

## Not supported here

* **Exporting.** The backend's own `nmcli connection export` writes the
  profile, private key included, to a 0600 file. This editor offers no export
  from Plasma; see [limitations.md](limitations.md#exporting-a-profile-is-refused).
* **Detecting the backend version.** There is no interface that says which
  backend is installed. Against 0.1.0, the editor will look like it worked
  and the connection will not come up.

## Where the evidence is

This repository's tests run against the real upstream libnm importer, built
from the pinned revision inside the test image, which records it in
`/backend-revision.txt`; with that backend present a skipped importer test is
a failure. The backend's own front-end conformance suite runs against this
editor through `autotests/openvpn3conformancedriver.cpp`; see
[verification.md](verification.md#the-backends-conformance-suite).

No run in this repository has activated a connection through a real
NetworkManager and openvpn3; see [limitations.md](limitations.md#what-has-not-been-shown).
