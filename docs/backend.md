# The backend this needs, and what is upstream

This module is the Plasma editor. The thing that actually connects is the
NetworkManager OpenVPN 3 backend, which provides the
`org.freedesktop.NetworkManager.openvpn3` service, its
`nm-openvpn3-service.name` file and the libnm editor plugin this module's
importer calls:

<https://github.com/AlexeySetevoi/network-manager-openvpn3>

## The short version

**This module needs a backend change that is not upstream yet.** Against the
released backend, connections it creates will not activate. That is not a
detail to discover later, so it is the first thing on this page.

## What is upstream, and what is not

Upstream's `main` is at `0f54141` — "Web login, PKCS#12, encrypted keys, GTK 3
editor; release 0.1.0". It stores a profile as a **public connection data
item**, `vpn.data["profile"]`, and knows nothing about any other layout.

Three commits are proposed upstream and not merged. They are
`AlexeySetevoi/network-manager-openvpn3` pull request #1, whose head is
`d919634b20e784c3a16544ca8b2c9254f4563f62`; upstream's `main` does not contain
them. Until it does, they are available from
<https://github.com/manicminer/network-manager-openvpn3> on
`feature/plasma-secrets`.

| Commit | What it adds |
| --- | --- |
| `9988c4b` | Self-contained profiles stored as a NetworkManager **secret**: `vpn.data["profile-storage"] = "secret"`, `vpn.data["profile-flags"]` = 1 or 0, `vpn.secrets["profile"]`. Also inlining file references inside `<connection>` scopes, and failing closed rather than reading a stale public copy. |
| `6fe03bc` | Dropping comments when importing a profile. |
| `d919634` | Dropping formatting-only blank lines when importing a profile. |

## What works against which backend

| | Released `0.1.0` (upstream `main`) | With pull request #1 |
| --- | --- | --- |
| The OpenVPN 3 connection type appears in Plasma | yes | yes |
| The editor opens, edits and saves a profile | yes | yes |
| **Import** through the backend's own libnm importer | yes | yes |
| A profile kept in the **user's wallet** (the default) | **the backend cannot read it** | yes |
| A profile kept as a **system secret** | **the backend cannot read it** | yes |
| A profile kept as a public data item (legacy) | yes | yes |
| An imported profile arrives without comments or blank lines | no — it keeps them | yes |
| File references inside a `<connection>` scope are inlined | no | yes |

The two rows in bold are why the short version says what it does. Secret
storage is this module's default for every new and every imported connection,
and public storage is offered **only** to a connection that already uses it —
deliberately, because a profile carries its private key and NetworkManager
hands connection data to every client allowed to read the connection's
settings, while secrets go only to the owning agent or on an explicit request.
So against a released backend there is no way to create a connection from this
editor that the backend can read. The editor will look like it worked; the
connection will not come up.

Nothing in this module detects which backend is installed. There is no
interface that would answer the question, and guessing would be worse than
this page.

## Where the evidence for the backend is

Not here. This repository's tests cover the Plasma side and the real libnm
importer; the test image installs the backend's libnm plugin so that the
importer tests exercise it rather than skip, and the image records the backend
revision it was built from in `/backend-revision.txt`.

End-to-end evidence — real NetworkManager and openvpn3 activation, traffic,
DNS, disconnect and reuse for system-owned secret profiles, PEM, PKCS#12 and
legacy cases — belongs to the backend repository and was produced there,
against a different distribution's NetworkManager and openvpn3 packages. It is
not reproduced here and it does not by itself certify any particular
distribution's versions. No run has combined this Plasma editor, a wallet and
a live NetworkManager activation; see [limitations.md](limitations.md).
