# Patches for plasma-nm itself

**None of these are applied by this project's build, and none of them are in
any package this project produces.** They are not upstream. They are carried
here because three of the limitations in [../docs/limitations.md](../docs/limitations.md)
are limitations *of the host*, and this is what lifting them looks like — for
anyone building their own plasma-nm, and as the concrete proposal behind the
pending upstream change.

The module works against a stock plasma-nm without any of them. What each one
buys is in the table below.

| Patch | Lifts |
| --- | --- |
| `0001-offer-an-ipv6-page-and-request-secrets-for-openvpn3.patch` | The missing **IPv6** page for OpenVPN 3 connections, and a `GetSecrets` request for a secret-mode profile whose `profile-flags` key some other writer omitted. |
| `0002-preserve-connection-properties-the-editor-does-not-show.patch` | Connection properties being dropped on every save by the editor's own General page. This is a defect of stock plasma-nm for **every** connection type and has nothing to do with OpenVPN 3. |
| `0003-ask-which-vpn-plugin-should-import-a-shared-extension.patch` | The top-level **Import VPN** picking whichever plugin claims `.ovpn` first. With it, the import asks which of OpenVPN and OpenVPN 3 was meant; without it, this module stays out of the way entirely and OpenVPN 2 keeps every `.ovpn`. |

They are plain `git diff` output against plasma-nm **6.7.5**
(`af9d0f5386a0a2c340e1392689795b2fdbd60035`), and apply with:

```bash
git -C /path/to/plasma-nm apply /path/to/host-patches/0001-*.patch
```

A plasma-nm built with any of them is no longer the distribution's plasma-nm.
This module must then be built and tested against that build, and the pin and
digests re-derived from it, or the ABI check will be comparing against the
wrong thing. See [../docs/repinning.md](../docs/repinning.md).

## Copyright

These diffs touch files that are
`SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL`,
copyright Jan Grulich, Lukáš Tinkl, Will Stephenson and the KDE community;
the changes within them are copyright 2026 Tom Bamford under the same terms as
the files they modify. Full texts are in [../LICENSES/](../LICENSES/).
