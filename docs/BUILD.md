# Building the PS4 package

The build is a patch-based cross-build: Kodi is cloned at the commit in
`port/base.conf`, the port series is applied, and the result is compiled
with the OpenOrbis toolchain and packaged as a PKG.

## Requirements

- Linux host (the OpenOrbis toolchain is Linux-only; use Docker/WSL on
  macOS or Windows).
- `git`, `curl`, `tar`, `clang`, `cmake`, `make`, `ar`, `python3`,
  `pkg-config`, `glslc` and the PCRE2 development files (`libpcre2-8`).
- A PS4 with homebrew support for installing the resulting PKG.

`./build.sh --all` downloads the OpenOrbis toolchain when it is not found,
clones Kodi, builds the host and target dependencies, applies the port,
builds and packages. Set `PS4SDK`/`OO_PS4_TOOLCHAIN` to use an existing
toolchain.

## One-command build

```bash
git clone https://github.com/washaka81/KODI-for-PS4.git
cd KODI-for-PS4

# Validated 0.1.2-alpha1 development series
./build.sh --all --diag

# Release series
./build.sh --all
```

The first run downloads the toolchain (~152 MB) and builds the target
dependencies, so it takes a while. Later builds are incremental:

```bash
./build.sh
```

The PKG is written to `dist/kodi-ps4-<version>.pkg`; the script prints its
size and SHA-256 and runs the privacy check before packaging.

## Options

```text
--all                 install missing toolchain/deps, clone Kodi and build
--diag                use the development series (PORT_DIR=port/diag)
--port-dir DIR        port series directory (default: port)
--kodi-dir DIR        Kodi checkout (default: src/kodi)
--check               validate toolchain and dependencies only
--bootstrap           recreate the dependency tree
--clean               remove the build tree and staging directory
--build-dir DIR       build tree
--toolchain DIR       OpenOrbis toolchain
--deps DIR            target libraries
--jobs N              parallel jobs
--tag                 create the local release tag after building
--push --validated    push the current branch and tags to origin
```

## Build tree

The default build tree is `build/kodi-ps4`; an existing
`build/kodi-ps4-openssl` tree is preferred for the external-FFmpeg setup
used by the current packages. Build outputs and `dist/` are ignored by git.

The validated live packages use the external-FFmpeg tree
(`ENABLE_INTERNAL_FFMPEG=OFF`, `DEPS_PS4=$HOME/kodi-deps-ps4-openssl`).
An internal-FFmpeg tree does not contain the `ps4http` protocol or the
`h264_videodec2` decoder; a package built from it can play local video
with software decoding but cannot validate HLS/live playback. Keep the
external tree for console validation until the build script has a
dedicated external-FFmpeg option.

## Dependencies

The dependency setup is idempotent and lives outside the repository under
`$HOME/kodi-build-deps` and `$HOME/kodi-deps-ps4`; symlinks under
`/tmp/kodi-ps4` and `/tmp/kodi-deps-ps4` keep the CMake/FFmpeg paths
stable across reboots. `./build.sh --bootstrap` recreates them.

The embedded CPython build is part of the dependency setup. If it is not
available, the core still builds with `ENABLE_PYTHON=OFF`; re-run the
bootstrap to enable Python add-ons.

## Packaging

`./build.sh --all` calls `scripts/package-port.sh`, which applies and
verifies the port, builds the GLES shim, links the PVR add-on, creates the
self/eboot, validates the PKG and writes the versioned artifact. Packaging
requires `PkgTool.Core` from the OpenOrbis toolchain; some distributions
need an OpenSSL 1.1 compatibility library in `LD_LIBRARY_PATH`.

## Updating Kodi

`port/README.md` documents the patch capture/check/apply workflow and the
`./scripts/kodi-port.sh update origin/master` path for moving the base to a
new official Kodi release.
