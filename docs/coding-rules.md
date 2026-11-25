# Coding rules — Kodi for PS4

## Scope

- Keep the port close to upstream Kodi. Changes belong in `port/`, not in
  long-lived local commits inside `src/kodi`.
- Port code is a translator layer: GLES2/EGL over `sceGnm`/`sceVideoOut`,
  audio, input, filesystem, memory and network glue. Do not change Kodi
  UI or playback logic unless the change is a documented platform
  exception.
- Target firmware range: 5.05–13.52 with homebrew support. Do not depend
  on proprietary Sony SDK blobs.

## Build and packaging

- Build with the OpenOrbis toolchain.
- Define `PS4`, `__ORBIS__` and `ORBIS` on PS4 builds.
- Use the self/eboot flags in `scripts/package-port.sh` and always run
  `pkg_validate`.
- Keep the PKG generic: no user M3U/EPG lists, logos, credentials, console
  identifiers or memory settings for a specific console.

## Code style

- Platform code lives under `xbmc/platform/ps4/`.
- Use fixed-width types and explicit constructors.
- Avoid dynamic allocation in hot paths.
- Log every non-trivial platform error code.
- Keep new code and comments in English.

## Rendering and video

- The primary path is the GLES shim over GNM; Piglet is not used for Kodi.
- The video renderer must validate buffer size, pitch, alignment, rows and
  crop bounds before writing.
- BT.709 and BT.601 sources use the base YUV2RGB shader and the source YUV
  matrix. Do not reintroduce a separate primaries-conversion program
  without a console test.
- Preserve the 896-pixel packed FBO and final crop unless the decoder
  alignment changes.

## Audio and A/V sync

- The PS4 sink is 48 kHz stereo over `sceAudioOut`.
- Live streams use audio-master clock feedback; do not force the resample
  sync loop without a console A/B.
- Keep the startup anchor and the rebuffer watchdog when changing playback
  code.

## Diagnostics

- Canaries, allocation traces, memory maps and verbose probes belong in
  the development series (`port/diag`), not in the release series.
- Do not ship logs, captures, databases or console-specific files.

## Before submitting

- Run `./scripts/privacy-check.sh`.
- Run `./scripts/kodi-port.sh check` on a clean base.
- Validate the PKG on console and review the logs before publishing.
