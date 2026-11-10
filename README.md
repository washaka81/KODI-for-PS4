# KODI for PS4

A native port of Kodi to the PlayStation 4, built with the OpenOrbis
toolchain and a small GLES2/EGL shim on top of **sceGnm + sceVideoOut**
(no Piglet/OpenGL).

The Kodi source tree is **not vendored**. The port is kept as a patch series
in `port/` and applied to a clean Kodi checkout; `src/kodi` is a disposable
working tree. This keeps the port close to upstream and makes updates
reviewable.

## Status

- Kodi core builds, links and starts on the PS4.
- The Estuary GUI renders through the GNM/GLES shim; text, menus and the
  controller work.
- Frame pacing is vblank-locked at ~59.9 fps with 0 timeouts.
- Audio is registered as a timed sink but still silent (`sceAudioOut` is not
  implemented yet).
- Video playback and hardware decode are experimental.

## Requirements

- OpenOrbis PS4 toolchain (`OO_PS4_TOOLCHAIN`, e.g. `/opt/ps4/sdk`).
- Host tools: `clang`, `cmake`, `ninja`/`make`, `python3`, `git`, `glslc`.
- A Kodi source checkout at the commit in `port/base.conf`.
- For packaging: `PkgTool.Core` from the OpenOrbis toolchain. `pkg_build`
  may need OpenSSL 1.1; if your distribution does not provide it, point
  `LD_LIBRARY_PATH` at a compatible copy.

## Quick start

```bash
git clone https://github.com/washaka81/KODI-for-PS4.git
cd KODI-for-PS4

# Kodi source at the port base
git clone https://github.com/xbmc/xbmc.git src/kodi
git -C src/kodi checkout 85cd79a00cfaff6212cf5265c91f92fddb9846a5

export OO_PS4_TOOLCHAIN=/opt/ps4/sdk

# Ephemeral build dependencies
./scripts/setup-psbc.sh
./scripts/setup-m1-deps.sh

# Configure the Kodi cross-build
./scripts/configure-kodi-ps4.sh "$HOME/kodi-build-ps4"

# Build and package
BUILD_DIR="$HOME/kodi-build-ps4" ./scripts/package-port.sh
```

The resulting package is written to `dist/`. Install it with your usual PS4
package installer. No prebuilt binaries or packages are stored in this
repository.

## Port layout

```text
port/base.conf   upstream Kodi commit the port is applied on
port/patches/    ordered patch series (plain diffs)
port/series      patch order
port/overlay/    optional new files not covered by the patches
scripts/         build, packaging and port-maintenance helpers
tools/gnm-engine GLES2/EGL shim and GNM test engine
cmake/           PS4 toolchain files
```

## Updating Kodi

```bash
./scripts/kodi-port.sh update origin/master
```

The command tests the patch series on the new base and only updates
`base.conf` when every patch applies. If there are conflicts it leaves a
worktree to resolve and prints the `finalize` command. See `port/README.md`
for details.

## License

Kodi is licensed under GPL-2.0-or-later. This repository is distributed
under the GNU General Public License v3.0; see `LICENSE`. Third-party
components in `tools/gnm-engine/shared/src/libs` keep their own licenses.
