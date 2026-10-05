# plasma-nm-openvpn3

A native **OpenVPN 3** connection editor for Plasma's network configuration,
built as a separate module that loads into an **unmodified distribution
plasma-nm**.

It edits `org.freedesktop.NetworkManager.openvpn3` connections: general fields,
the full ordered directives table and a raw profile source view, all three
views of one profile document. A profile carries its private key inlined, so
every new and every imported connection keeps it where only an agent or an
explicit request can get at it: the user's wallet, or a NetworkManager
system-owned secret. The older layout — the profile as a public connection data
item, which NetworkManager hands to every client allowed to read the
connection's settings — is offered only to a connection that already uses one,
so that opening such a connection and saving it does not move its keys without
being asked; moving it is a deliberate choice on the editor page. Importing an
existing `.ovpn` profile goes through the OpenVPN 3 backend's own libnm
importer, so what Plasma stores is what the backend will read.

This repository contains the Plasma editor only. It does not modify, replace or
ship any part of plasma-nm.

> **Read [docs/backend.md](docs/backend.md) first.** This module needs a
> NetworkManager OpenVPN 3 backend change that is **not upstream yet**; against
> the released backend, connections it creates will not activate.

| | |
| --- | --- |
| What it does not do | [docs/limitations.md](docs/limitations.md) |
| The backend it needs | [docs/backend.md](docs/backend.md) |
| How it is verified | [docs/verification.md](docs/verification.md) |
| Moving to another plasma-nm | [docs/repinning.md](docs/repinning.md) |
| Patches for plasma-nm itself | [host-patches/](host-patches/) |

## Compatibility

| | Supported |
| --- | --- |
| plasma-nm | **6.7.5 only** — see below, and [docs/repinning.md](docs/repinning.md) |
| Verified distribution | Arch Linux, `plasma-nm 6.7.5-1`, x86_64 |
| Qt / KF6 | 6.11.2 / 6.30.0 as verified; ≥ 6.10 / ≥ 6.26 required |
| NetworkManager | `libnm` > 1.4 |
| Backend | `network-manager-openvpn3` with pull request #1 ([docs/backend.md](docs/backend.md)) |

Other distributions are expected to work where their plasma-nm is 6.7.5; none
has been verified, and the build refuses to configure against any other
version, so a wrong one is a failed build rather than a crash later.

## Why it needs plasma-nm's source to build

plasma-nm installs `libplasmanm_editor.so` — the library that defines
`VpnUiPlugin`, `SettingWidget` and `PasswordField` — but installs none of its
headers, gives the library no `SOVERSION`, exports no ABI tag and ships no
pkg-config file. There is therefore no supported way to build against it, and
no way for a wrongly built module to fail safely: it would load and then crash
the connection editor.

This module deals with that by being pinned, verified and fail-closed:

* The four headers it uses come from one immutable upstream revision,
  `af9d0f5386a0a2c340e1392689795b2fdbd60035` — KDE's `v6.7.5` tag — recorded in
  [`provenance/plasma-nm-6.7.5.pin`](provenance/plasma-nm-6.7.5.pin).
* Their SHA-256 digests are committed in
  [`provenance/plasma-nm-6.7.5.sha256`](provenance/plasma-nm-6.7.5.sha256) and
  checked on every configure, whether the source tree was fetched or already
  present.
* The build asks the distribution's package database which package owns the
  installed `libplasmanm_editor.so` and what version it is, and **refuses to
  configure** unless that is exactly the pinned version.
* Everything is linked with `-Wl,--no-undefined`, so a symbol that moved is a
  failed build rather than a crash at runtime.
* No upstream source is vendored here, nothing is downloaded while building
  once the source tree is present, and nothing is ever downloaded at runtime.

## Building

Build dependencies: a C++20 compiler, CMake ≥ 3.16, `extra-cmake-modules`
≥ 6.26, Qt 6 ≥ 6.10 (Core, DBus, Gui, Widgets), KF6 ≥ 6.26 (CoreAddons, I18n,
NetworkManagerQt, WidgetsAddons), `qtkeychain-qt6` ≥ 0.16, `libnm` > 1.4, and
the installed `plasma-nm` whose editor library is linked against.

```bash
scripts/fetch-plasma-nm-source.sh          # one network step, verified against the pin
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
```

`fetch-plasma-nm-source.sh` writes `./plasma-nm-source`, which is where the
build looks without being told. If you already have a plasma-nm 6.7.5 checkout,
point at it instead and skip the network entirely:

```bash
cmake -S . -B build -DPLASMA_NM_SOURCE_DIR=/path/to/plasma-nm
```

Its digests are verified either way.

## Installing

```bash
sudo cmake --install build
```

That installs exactly one file,
`<prefix>/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_openvpn3ui.so`,
next to the distribution's own VPN plugins. Nothing of plasma-nm's is replaced
or shadowed, and removing that one file removes the module.

A release also publishes a binary archive of that single file, for the
distribution and plasma-nm version in its own filename, with its provenance and
checksums beside it. It is good for that plasma-nm and nothing else; the
archive's own `PROVENANCE.txt` says so and says what it was built against.

Log out and back in, or restart the connection editor, and **OpenVPN 3**
appears in the list of VPN types — provided the backend is installed, since
that is what provides the service file the type is discovered from.

## Using it

Add a VPN connection of type **OpenVPN 3**, then either write a profile on the
**Profile Source** page or use **Import Profile…** on the editor page to read
an existing `.ovpn` file.

**Import Profile… is the way in.** This module deliberately does not appear in
the connection editor's own top-level *Import VPN connection*, because on a
stock host that would mean sometimes stealing an OpenVPN 2 import; the reason
is in [docs/limitations.md](docs/limitations.md). Importing from the editor
page is also the only path that can ask where the profile should be kept.

**Profile storage** decides who can read the private keys: the user's wallet
(the default), or NetworkManager for all users, which is what an unattended
connection that must come up with nobody logged in needs. A connection that
already keeps its profile in the older public layout also offers that, labelled
as unprotected, and keeps it until you choose otherwise — the editor does not
move somebody's stored keys as a side effect of opening the page, and it does
not offer that choice to anything else. The editor refuses to
save a wallet-stored profile when there is no wallet to store it in, rather than
quietly storing it somewhere else, and refuses to save at all when it was not
given the profile it is supposed to be editing.

There is no export. An OpenVPN 3 profile has the private key inlined in it, and
when it is kept with the connection's secrets that is precisely so it never
reaches a plain file.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

The container harness in [`testing/`](testing/) runs the same build and tests
against a stock, unpatched distribution plasma-nm in a disposable image, with
no network, no host bus, no host home directory and no real credentials:

```bash
export BUILD_ROOT=/path/to/scratch/openvpn3-build        # dedicated, outside the checkout
export BACKEND_ROOT=/path/to/network-manager-openvpn3    # backend checkout, read only
testing/run.sh images        # once; the only step that installs packages
testing/run.sh source
testing/run.sh build
testing/run.sh test
testing/run.sh package
testing/run.sh install-check
```

`BACKEND_ROOT` is optional and worth having: every assertion that needs the
real libnm importer is guarded, so **a run with no skips is the evidence that
the real importer was exercised**, and a run without the backend is not one.
See [docs/verification.md](docs/verification.md) for what those containers are
and are not allowed to do.

## Licensing

This module is `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`,
the licence of the plasma-nm VPN plugins it was developed as one of. Full texts
are in [`LICENSES/`](LICENSES/).

The pinned plasma-nm headers it compiles against are
`LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL`, copyright
their authors (Will Stephenson, Lukáš Tinkl, Jan Grulich and the KDE
community). They are read from a verified upstream source tree at build time
and are not redistributed by this project. The diffs in
[`host-patches/`](host-patches/) touch files under the same licence and are
documented there.
