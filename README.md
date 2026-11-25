# KODI for PS4

**Release base: `0.1.2`** (Kodi 22.0 RC2 Piers). Package versions are
independent of the Kodi version; diagnostic iterations are tracked in
`CHANGELOG.md`. Nothing is published until it passes console validation.
See `VERSION`, `CHANGELOG.md` and `docs/STATUS.md`.

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
- Video uses the hardware H.264 decoder (Videodec2) on the video plane;
  854x480 (SD/BT.601) and 1080p streams render through the same YUV2RGB
  image path at ~59.9 fps. The live SD/BT.601 case is validated end to end.
- Audio is stereo PCM through `sceAudioOut`. Live streams use the audio
  clock as the A/V master (clock feedback); startup and rebuffer recovery
  are handled without the previous ~1 s resampler oscillation.
- Network works: native `libSceHttp`/`libSceSsl` transport for FFmpeg
  HTTP(S) (HLS, DASH, MP4, MPEG-TS); Curl keeps OpenSSL for M3U, logos,
  EPG and addons. The native client is the one accepted by the live CDN
  WAF; OpenSSL and mbedTLS are reset after the master. Toggle:
  `/data/kodi-ps4/native-http` or `native-https`. See
  `docs/ARCHITECTURE.md`.
- System Info reads real memory, CPU usage/frequency/temperature and
  storage through `statvfs("/user")` (no AppContent, which could crash).
- First boot builds a hardware/TV profile: model, firmware, language,
  date/time format, time zone, HDMI mode, safe area, HDR10 capability and
  network. Kodi applies the console `TZ` and a matching locale.
- `pvr.iptvsimple` 22.6.5 (PVR API 9.2.0) is packaged and usable for
  M3U/HLS/TS. DASH/RTMP needs optional `inputstream.*` addons that are not
  built yet.
- PS4 applications can be launched from Kodi through the native builtin
  `LaunchPS4App(titleId)` (`sceLncUtilLaunchApp`), plus the
  `script.ps4.launcher` addon.
- Python support works: CPython 3.14.6 is cross-compiled for Orbis, Kodi
  links it statically and the package ships the stdlib. Services and
  plugins run (`interpreter ready` in ~70-162 ms). See
  `docs/ARCHITECTURE.md`.
- The GLES shim is built with `-O2 -march=btver2` and SSE/SSSE3/SSE4.1
  routines for texture uploads and font UVs. It is smaller than the
  `-O0` build. See `docs/ARCHITECTURE.md`.

### 0.1.2-alpha1

This alpha consolidates the validated SD/BT.601 work:

- The BT.601 SD channel uses the same base YUV2RGB shader as the BT.709
  channels, with the source YUV matrix and the 896-pixel packed FBO.
- Live A/V sync anchors the clock to the real audio in flight and uses
  audio-master clock feedback; the rebuffer watchdog flushes and re-runs
  the start sync after a long stall.
- Static calibration patterns (1080p/4K brightness, colour and contrast)
  are available in the local calibration folder; the image path is
  measured within ±2 levels of the source.
- Pending: exact linear BT.601→BT.709 primaries conversion, the `flex`
  memory pressure under the 395-channel workload, 4K output/HDR, and
  full add-on coverage. The release series promotion is pending; this
  alpha is built from the development series.

The PKG is generic; test M3U/logos are user data and are not shipped.
See `docs/STATUS.md`, `docs/ARCHITECTURE.md`, `docs/BUILD.md` and
`CHANGELOG.md`.

## Documentation

- Status and known limitations: `docs/STATUS.md`
- Architecture and translator layers: `docs/ARCHITECTURE.md`
- Build and packaging: `docs/BUILD.md`
- Release process: `docs/RELEASE.md`
- Coding rules: `docs/coding-rules.md`
- Community standards: `docs/COMMUNITY-STANDARDS.md`
- Privacy: `PRIVACY.md`
- Port workflow: `port/README.md`
- Changelog: `CHANGELOG.md`

## Requirements

- Linux host (the OpenOrbis toolchain is Linux-only; use Docker/WSL on
  macOS/Windows).
- Host tools: `git`, `curl`, `tar`, `clang`, `cmake`, `make`, `ar`,
  `python3`, `pkg-config`, `glslc` and the PCRE2 development files
  (`libpcre2-8`) used by the host SWIG build.
- `./build.sh --all` downloads the OpenOrbis toolchain when it is not
  found, clones Kodi at the commit in `port/base.conf`, bootstraps SWIG
  and the target libraries, applies the port series, builds and packages.
- To use an existing toolchain, set `PS4SDK`/`OO_PS4_TOOLCHAIN`.
- For packaging: `PkgTool.Core` from the OpenOrbis toolchain. `pkg_build`
  may need OpenSSL 1.1; if your distribution does not provide it, point
  `LD_LIBRARY_PATH` at a compatible copy.

## Quick start

```bash
git clone https://github.com/washaka81/KODI-for-PS4.git
cd KODI-for-PS4

# One command: toolchain + Kodi + dependencies + port + build + PKG.
# --diag builds the validated 0.1.2-alpha1 development series.
./build.sh --all --diag
```

Use `./build.sh --all` for the release series, and a later `./build.sh`
for incremental builds. The first run downloads the toolchain (~152 MB)
and builds the target dependencies, so it takes a while.

The PKG is written to `dist/kodi-ps4-<version>.pkg` and its SHA-256 is
printed. The script runs the privacy check before packaging and never
includes `userdata`, M3U/EPG lists, logs, databases or keys. No prebuilt
binaries are stored in the repository.

### Operating systems

- **Linux:** native (the only platform supported by the OpenOrbis
  toolchain).
- **Windows:** run inside **WSL2** (Ubuntu). Native Git Bash cannot use the
  Linux PS4 toolchain.
- **macOS:** use Docker with a Linux image that provides `clang`, `cmake`,
  `make`, `python3` and the mounted toolchain:
  ```bash
  export PS4SDK=/path/to/PS4Toolchain
  export KODI_PS4_DOCKER_IMAGE=your-linux-image
  ./build.sh
  ```
  Without Docker, use a Linux VM.

### GitHub and versions

`./build.sh --tag` creates the local `v<version>` tag. Publishing to GitHub
is only done with `./build.sh --push --validated`, after testing the PKG on
console and reviewing the logs. The script never publishes user data; see
`PRIVACY.md`.

## Port layout

```text
port/base.conf   upstream Kodi commit the port is applied on
port/patches/    ordered patch series (plain diffs)
port/series      patch order
port/overlay/    new files not covered by the patches
port/addons/     packaged addons that are not part of the Kodi tree
scripts/         build, packaging and port-maintenance helpers
tools/gnm-engine GLES2/EGL shim and GNM test engine
cmake/           PS4 toolchain files
```

`port/addons/script.ps4.launcher` is copied into the package by
`scripts/package-port.sh`; `scripts/build-pvr-iptvsimple.sh` installs
`pvr.iptvsimple` with its full `resources/` directory.

## PVR / IPTV

`pvr.iptvsimple` is built out-of-tree from a pinned commit
(`f917044`, version 22.6.5). The package includes the addon and its
`resources/instance-settings.xml`, so **Configure** shows the General,
EPG, Channel logos, Timeshift and Advanced categories.

`scripts/build-pvr-iptvsimple.sh` builds the addon and installs it into the
Kodi build tree; `scripts/package-port.sh` calls it automatically. It needs
`pugixml` and `liblzma` for the target, which `scripts/setup-m1-deps.sh`
builds.

Enable it in **Settings → PVR & TV → PVR clients**, then configure the M3U
URL/path and the optional XMLTV EPG URL. The internal FFmpeg player handles
HLS/TS; DASH and RTMP require `inputstream.adaptive`/`inputstream.ffmpegdirect`
and are not packaged yet. See `docs/STATUS.md` for details.

## Python addons

Kodi embeds CPython; it does not compile Python plugins.
`scripts/build-python-ps4.sh` cross-compiles CPython 3.14.6 for Orbis into
`/tmp/kodi-deps-ps4` (static `libpython`, headers and stdlib), and
`scripts/configure-kodi-ps4.sh` enables `ENABLE_PYTHON=ON` automatically
when it finds it. The package ships the stdlib under
`system/python/lib/python3.14`; `XBPython` sets `PYTHONHOME` to that path.

`_ssl`, `_hashlib`, `_socket`, `_sqlite3`, `zlib`, `_lzma`, `_decimal`,
`pyexpat`, `_elementtree`, `hashlib` and the usual pure-Python stdlib are
included. Known pending modules are `_ctypes` (libffi) and `_uuid`. See `docs/ARCHITECTURE.md`.

The native `LaunchPS4App` builtin works without Python and can be bound to
a key or called from a favorite/JSON-RPC.

## Updating Kodi

```bash
./scripts/kodi-port.sh update origin/master
```

The command tests the patch series on the new base and only updates
`base.conf` when every patch applies. If there are conflicts it leaves a
worktree to resolve and prints the `finalize` command. See `port/README.md`
for details.

## Community

- Contributing: `CONTRIBUTING.md`
- Code of conduct: `CODE_OF_CONDUCT.md`
- Security policy: `SECURITY.md`
- Privacy and data policy: `PRIVACY.md`
- License and third-party notices: `LICENSE`, `NOTICE.md`
- Issue and pull-request templates: `.github/`
- Community standards status: `docs/COMMUNITY-STANDARDS.md`

## License

Kodi is licensed under GPL-2.0-or-later. This repository is distributed
under the GNU General Public License v3.0; see `LICENSE` and `NOTICE.md`.
Third-party components in `tools/gnm-engine/shared/src/libs` and
`tools/liborbispkg` keep their own licenses. Contributions are accepted
under the repository license.
