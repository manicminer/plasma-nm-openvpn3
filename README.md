# plasma-nm-openvpn3

A native **OpenVPN 3** connection editor for Plasma's network configuration,
built as a separate module that loads into an **unmodified distribution
plasma-nm**.

It edits `org.freedesktop.NetworkManager.openvpn3` connections: general fields,
the full ordered directives table and a raw profile source view, with
self-contained profiles stored either in the user's wallet or as a
NetworkManager system secret. Importing an existing `.ovpn` profile goes
through the OpenVPN 3 backend's own libnm importer, so what Plasma stores is
what the backend will read.

This repository contains the Plasma editor only. It needs the companion
NetworkManager OpenVPN 3 backend at runtime — see
[Backend requirements](#backend-requirements) — and does not modify, replace or
ship any part of plasma-nm.

> **Status:** initial release in progress.

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
  configure** unless that is the pinned version.
* Everything is linked with `-Wl,--no-undefined`, so a symbol that moved is a
  failed build rather than a crash at runtime.
* No upstream source is vendored here, nothing is downloaded while building
  once the source tree is present, and nothing is ever downloaded at runtime.

Supported host: **plasma-nm 6.7.5**. Other versions need a re-pin; see
[docs/repinning.md](docs/repinning.md).

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

## Testing

```bash
ctest --test-dir build --output-on-failure
```

At this point that is the pinned-source verification suite. A container
harness that builds and tests against a stock plasma-nm follows.

## Backend requirements

At runtime this module needs the NetworkManager OpenVPN 3 backend, which
provides the `org.freedesktop.NetworkManager.openvpn3` service, its
`nm-openvpn3-service.name` file and the libnm editor plugin the importer calls:
<https://github.com/AlexeySetevoi/network-manager-openvpn3>.

Self-contained profiles stored in NetworkManager secrets — the default storage
here — need backend support that is **not upstream yet**.

## Licensing

This module is `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`,
the licence of the plasma-nm VPN plugins it was developed as one of. Full texts
are in [`LICENSES/`](LICENSES/).

The pinned plasma-nm headers it compiles against are
`LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL`, copyright
their authors (Will Stephenson, Lukáš Tinkl, Jan Grulich and the KDE
community). They are read from a verified upstream source tree at build time
and are not redistributed by this project.
