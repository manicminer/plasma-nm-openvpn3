# Re-pinning to another plasma-nm version

This module is built against the private headers of one exact plasma-nm
revision and refuses to build against an installed plasma-nm of any other
version. That is not caution for its own sake: `libplasmanm_editor.so` has no
`SOVERSION` and no ABI tag, so a module built against different declarations
of `VpnUiPlugin`, `SettingWidget` or `PasswordField` still loads, and then
misbehaves or crashes inside the connection editor. There is no runtime check
that could catch it, so the check is made at build time and made total.

Re-pinning is therefore a deliberate act with a verification step, not a
configuration option.

## What a pin consists of

Two files per supported version:

* `provenance/plasma-nm-<version>.pin` — the upstream repository, the immutable
  commit, the tag it corresponds to, the version, and the licence of the
  headers it covers.
* `provenance/plasma-nm-<version>.sha256` — SHA-256 digests of exactly the
  header files the build reads, in `sha256sum` format and relative to the
  source tree root.

The commit is what makes the pin immutable: a git commit hash covers the whole
tree it names, so fetching it from any mirror either yields these files or
fails. The digests are checked again regardless of where the tree came from, so
a local checkout is verified as strictly as a fresh clone.

## Re-pinning

1. Find the upstream commit for the release you need, and check that it is the
   release and not somebody's branch of the same name:

   ```bash
   git ls-remote https://invent.kde.org/plasma/plasma-nm.git 'refs/tags/v6.7.6^{}'
   ```

   The peeled tag (`^{}`) is the commit; an annotated tag's own hash is not.

2. Fetch that tree and read the version out of it rather than trusting the tag
   name:

   ```bash
   git clone --depth 1 --branch v6.7.6 \
       https://invent.kde.org/plasma/plasma-nm.git /tmp/plasma-nm
   git -C /tmp/plasma-nm rev-parse HEAD        # must equal the commit above
   grep 'set(PROJECT_VERSION' /tmp/plasma-nm/CMakeLists.txt
   ```

3. Write the new pin file, copying the existing one and replacing `version`,
   `commit` and `tag`.

4. Regenerate the digests over the same four headers:

   ```bash
   cd /tmp/plasma-nm
   sha256sum libs/editor/connectioneditorbase.h \
             libs/editor/vpnuiplugin.h \
             libs/editor/widgets/passwordfield.h \
             libs/editor/widgets/settingwidget.h \
       > /path/to/plasma-nm-openvpn3/provenance/plasma-nm-6.7.6.sha256
   ```

5. Point the build at the new pin and configure against a host running that
   version:

   ```bash
   cmake -S . -B build \
       -DPLASMA_NM_PIN_FILE=provenance/plasma-nm-6.7.6.pin \
       -DPLASMA_NM_DIGEST_FILE=provenance/plasma-nm-6.7.6.sha256 \
       -DPLASMA_NM_SOURCE_DIR=/tmp/plasma-nm
   ```

6. **Diff the headers against the previous pin before trusting the result.**
   The build can only tell you that a symbol disappeared; it cannot tell you
   that a virtual function was inserted into the middle of a vtable or that a
   member was added to a class this module derives from. Both change the
   layout silently.

   ```bash
   git -C /tmp/plasma-nm diff v6.7.5 v6.7.6 -- libs/editor/vpnuiplugin.h \
       libs/editor/widgets/settingwidget.h libs/editor/widgets/passwordfield.h \
       libs/editor/connectioneditorbase.h
   ```

   An empty diff means the pin move is a formality. Anything else — a new
   virtual, a new data member, a changed signature, a changed base class —
   means the module has to be rebuilt and retested against that host, and the
   host compatibility tests re-read, before the version is claimed as
   supported.

7. Rebuild, run the full suite against a host of the new version, and record
   the result in the compatibility matrix in the README.

## A runtime range in a distribution package is not a wider pin

The pin governs the build: one exact version, verified, fail-closed, and that
default does not change. What a downstream package then declares as its runtime
dependency on plasma-nm is that package's own policy, and it may be wider.
The [lab/arch-packages build](https://code.manicminer.io/lab/arch-packages),
for instance, deliberately accepts any stable
`plasma-nm >= 6.8, < 6.9`, so that a plasma-nm patch update does not wait on a
rebuild of this module. To build against a 6.8.x host other than the pinned
one it passes `-DPLASMA_NM_ALLOW_ABI_MISMATCH=ON`, behind its own guard that
admits only a plain 6.8.x owned by the `plasma-nm` package, and keeps the
header digests and test suites mandatory. That knowingly departs from the
"must not be packaged" below. It is that package's decision, not something
this project vouches for.

The range rests on an assumption this project does not verify and does not
promise for future releases: that plasma-nm leaves the four private headers
alone within a patch series. It held from 6.8.0 to 6.8.1 and again from 6.8.1
to 6.8.2, where the four headers and the whole `libs/` tree are byte-identical,
and nothing more is known. Nothing can check it when the module is loaded, because the editor
library has no `SOVERSION` or ABI tag to compare against. So for each new
patch release inside such a range, run the header diff in step 6 against it;
an empty diff keeps the assumption true for that release, and anything else
means re-pinning, rebuilding and narrowing the range.

## Building against an unverified host anyway

`-DPLASMA_NM_ALLOW_ABI_MISMATCH=ON` turns the refusal into a warning. It
exists for bisecting and for looking at a crash on purpose. A module built
that way must not be installed on a machine anybody relies on and must not be
packaged; nothing checks that for you.

`-DPLASMA_NM_HOST_VERSION=<version>` is the narrower escape hatch, for a host
whose plasma-nm no package manager owns — a self-built one, or a container that
unpacked it by hand. It asserts the installed version rather than skipping the
comparison, so it still refuses to build if what you assert is not the pinned
version.
