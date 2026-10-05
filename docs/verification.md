# Reproducing the verification

Everything is built and run inside disposable Docker containers against a
**stock, unpatched distribution plasma-nm**, because that is the host this
module has to work on. Nothing in [`testing/`](../testing/) touches the machine
it runs on: no packages are installed outside Docker, no service is started, no
host D-Bus socket, home directory, wallet or `$XDG_RUNTIME_DIR` is ever
mounted, no real credentials exist anywhere in the fixtures, and the checkout
itself is mounted read only so a run cannot change the tree it is verifying.

## The two images

| Image | What it is |
| --- | --- |
| `plasma-nm-openvpn3-build-env` | `archlinux:latest`, the build dependencies, and the distribution's own **`plasma-nm` package**. Nothing here builds or replaces any part of plasma-nm. The image build fails if the distribution's `plasma-nm` is not the version the module is pinned to, so a rolling repository that has moved on is a loud early failure rather than a confusing one later. |
| `plasma-nm-openvpn3-test-env` | The above plus the OpenVPN 3 backend's own libnm plugin and its `nm-openvpn3-service.name`, built from a read-only checkout of the backend. This is what makes the importer tests exercise the real importer instead of skipping, and it records the backend revision it was built from in `/backend-revision.txt`. |

Only the image build installs packages, and only it and `run.sh source` use the
network at all.

## Running it

```bash
export BUILD_ROOT=/path/to/scratch/openvpn3-build        # dedicated, outside the checkout
export BACKEND_ROOT=/path/to/network-manager-openvpn3    # backend checkout, read only

testing/run.sh images        # only needed once, and the only step that installs packages
testing/run.sh source        # fetch and verify the pinned plasma-nm headers
testing/run.sh build
testing/run.sh test
testing/run.sh test -R openvpn3 -V
testing/run.sh screenshots   # not a test: renders the editor's pages for review
testing/run.sh package
testing/run.sh install-check
```

`BUILD_ROOT` is a dedicated scratch directory the harness owns: it creates it,
writes the fetched plasma-nm source and the build tree into it, and replaces
those directories on later runs. It is rejected if it resolves to the
filesystem root, a top-level directory, a home directory, or anywhere inside or
above the checkout — through `..` or a symlink as readily as directly.
`BACKEND_ROOT` is a read-only input and may not sit inside the directories the
harness replaces. [`testing/paths.sh`](../testing/paths.sh) holds these checks
and [`testing/test-paths.sh`](../testing/test-paths.sh) is their regression
suite; it runs `run.sh` against stub `docker`/`rsync`/`rm`/`mkdir`/`cp`/`git`,
so a guard regression fails an assertion instead of damaging the host:

```bash
testing/test-paths.sh
```

Those guards resolve a path before judging it, which is exactly why they
cannot see a workflow handing `actions/upload-artifact` a `BUILD_ROOT` with a
`..` in it — a build that passes every step and then fails on the upload.
[`testing/test-workflows.sh`](../testing/test-workflows.sh) reads the workflow
text instead: no `..` in a path given to an action, no step allowed to fail
softly, and a `BUILD_ROOT` the guards accept unchanged. It also runs
`actionlint` when that is installed, which CI requires:

```bash
testing/test-workflows.sh
```

`IMAGE_BUILD` and `IMAGE_TEST` override the image tags, so a second checkout
can be verified without overwriting the first one's images. `BUILD_TYPE`
defaults to `Debug`, `JOBS` to 2 and `MEMORY` to `8g`; containers run
unprivileged, with `--cap-drop ALL`, `--security-opt no-new-privileges`, no
swap beyond that memory limit and — for everything but `images` and `source` —
`--network none`.

## The release artifacts, and what checks them

`run.sh package` writes two things into `$BUILD_ROOT/package`, with a
`SHA256SUMS` over both:

* `plasma-nm-openvpn3-<version>.tar.gz` — the repository at one commit. It is
  what travels, and it builds anywhere the pin can be satisfied.
* `plasma-nm-openvpn3-<version>-<distro>-<arch>-plasma-nm-<host version>.tar.gz`
  — the module and nothing else, good for exactly the plasma-nm named in its
  own filename. The name is not decoration: plasma-nm's editor library has no
  versioned ABI, so a module built against another version loads and then
  misbehaves. The archive carries a `PROVENANCE.txt` saying what it was built
  against — source commit, plasma-nm package version, pin, build image digest,
  compiler, Qt and KF versions — its own `SHA256SUMS`, and the licence texts.

Both are built from the commit's own timestamp rather than the build's, so the
same commit packs to the same bytes: two consecutive `run.sh package` runs
produce byte-identical archives.

`run.sh install-check` then installs the binary archive the way its
`PROVENANCE.txt` tells a user to, inside a throwaway container, and checks what
that did:

* every file in the archive matches its checksum;
* exactly one file lands in the filesystem, and it is the VPN plugin;
* no path it installs is owned by a distribution package, and `pacman -Qkk`
  reports no distribution-owned file changed;
* the **installed** module is what loads — the test binaries are copied away
  from the build tree first, so the copy the ordinary test run uses cannot be
  what answers;
* the VPN plugins that were already installed, OpenVPN 2 and OpenConnect, are
  still there and still load alongside it;
* and removing that one file restores the host exactly.

It runs as root because installing into `/usr` is the thing being checked, in a
container with every capability dropped and no network, and the script refuses
to run outside a container at all.

## These images are not reproducible

They are built `FROM archlinux:latest` and installed from rolling
repositories, so rebuilding the same Dockerfile later produces a different
image with newer packages. What identifies the environment a result was
measured in is the recorded image digest and the package versions listed with
it, not the Dockerfile. `/image-packages.txt` inside the build image is the
full `pacman -Q` output at image build time; keep that and the digest alongside
any result you intend to rely on, and treat a rebuild as a new environment.
