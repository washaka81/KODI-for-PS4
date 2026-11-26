# Changelog

All published packages for the Kodi PS4 port. The Kodi base is always an
official release; the current base is Kodi 22.0 RC2 Piers
(`d686683a55d50e576dd282f53fb2bf9914931b38`).

## 0.1.2-alpha1 (2026-11-25)

**Validated on console.**

- The SD/BT.601 channel (854x480, H.264, SMPTE 170M) uses the base
  YUV2RGB image shader, the source YUV matrix and the 896-pixel packed
  FBO; the separate primaries-conversion program is disabled.
- A/V startup anchors the clock to the audio actually in flight, not to
  the theoretical ActiveAE maximum (`GENERAL_RESYNC` about -30 ms;
  initial error +23.6/-3.0 ms instead of +941.9 ms).
- Live playback uses audio-master clock feedback instead of changing the
  resample ratio; the periodic ±1 s oscillation is removed.
- Rebuffer recovery: if the presented video falls more than 2 s behind
  the clock, a watchdog flushes audio/video and re-runs the start sync
  (30 s cooldown).
- Calibration: 1080p/4K brightness, colour and contrast patterns; the
  image path measures within ±2 levels of the source.
- Native `libSceHttp`/`libSceSsl` transport for FFmpeg, Videodec2 hardware
  H.264 decode, PVR IPTV Simple, embedded CPython 3.14 and the packaged
  add-on launcher.
- Pending: a deliberate live-stall test of the rebuffer watchdog, exact
  linear BT.601→BT.709 primaries conversion, memory pressure under the
  395-channel workload, 4K/HDR output and full add-on coverage. The
  release series does not include these changes yet; this alpha is built
  from the development series.

## 0.1.1 (2026-11-14)

- First public PS4 port package: GLES2/EGL shim over GNM/VideoOut,
  audio and DualShock 4 input, network translators, PVR IPTV Simple,
  embedded Python and PKG packaging.
- Console bring-up, GUI rendering, local playback and the first network
  validation.

## 0.1.0 (2026-11-14)

- Initial repository and build tooling for the PS4 port.
