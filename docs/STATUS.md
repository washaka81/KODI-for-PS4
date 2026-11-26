# Project status

**Version:** `0.1.2-alpha1`  
**Kodi base:** 22.0 RC2 Piers (`d686683a55d50e576dd282f53fb2bf9914931b38`)  
**Platform:** PlayStation 4, OpenOrbis toolchain, FreeBSD 9 userland

## Validated on console

- Kodi core builds, links and boots; the Estuary GUI renders through the
  GLES2/EGL shim over `sceGnm`/`sceVideoOut`.
- DualShock 4 input, menus, text and the controller work.
- Stereo PCM audio through `sceAudioOut`.
- Hardware H.264 decode through Videodec2; 854x480 and 1080p streams
  render through the same YUV2RGB image path at ~59.9 fps.
- The SD/BT.601 live channel uses the base image shader, the source YUV
  matrix and the 896-pixel packed FBO; the separate primaries-conversion
  program is disabled.
- A/V startup anchors the clock to the audio actually in flight.
  `GENERAL_RESYNC` is about -30 ms and the initial ActiveAE error is in
  the tens of milliseconds instead of the previous +941.9 ms.
- Live playback uses audio-master clock feedback; the periodic ±1 s
  resampler oscillation is removed.
- Native `libSceHttp`/`libSceSsl` HTTP(S) transport for FFmpeg; Curl keeps
  OpenSSL for M3U, EPG and add-on traffic.
- `pvr.iptvsimple` 22.6.5 (PVR API 9.2.0) is packaged and usable.
- Embedded CPython 3.14.6 with the standard library; Python add-ons and
  services run.
- 1080p/4K static calibration patterns for brightness, colour and
  contrast; the image path measures within ±2 levels of the source.

## Known limitations

- Exact linear BT.601→BT.709 primaries conversion is still pending; the
  current image uses the source YUV matrix.
- Memory pressure (`flex`) under the full 395-channel workload is still
  being reduced. Image/add-on guards are temporary safety nets.
- 4K output and HDR10 are not enabled. HDR is only announced when the
  display, the 10-bit swapchain and BT.2020/PQ metadata are all available;
  otherwise Kodi tone-maps to SDR.
- Full coverage of binary and Python add-ons is still in progress.
- A rebuffer watchdog is implemented (flush plus start sync, 30 s
  cooldown), but it has not yet been exercised with a deliberate live
  stall.
- The release series does not include the latest validated changes yet;
  `0.1.2-alpha1` is built from the development series.
