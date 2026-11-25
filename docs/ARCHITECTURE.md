# Architecture

Kodi is not vendored: the repository keeps a patch series in `port/` and a
thin PS4 platform layer. `src/kodi` is a disposable checkout of Kodi 22.0
RC2 Piers.

## Layers

| Layer | Location | Purpose |
|---|---|---|
| Platform entry | `xbmc/platform/ps4/` | App lifecycle, filesystem, memory, time, system services |
| Window/EGL | `xbmc/windowing/ps4/` | EGL surface, video mode, flip rate, input loop |
| GLES→GNM shim | `tools/gnm-engine/` | Translates GLES2/EGL calls to `sceGnm` and `sceVideoOut` |
| Audio sink | `xbmc/platform/ps4/CAESinkPS4.*` | ActiveAE sink over `sceAudioOut` |
| Video decode | FFmpeg + `h264_videodec2` | Hardware H.264 decode through `libSceVdecCore` |
| Video render | `LinuxRendererGLES` | YUV2RGB shaders, packed FBO and final blit |
| Network | FFmpeg `ps4http` + Curl/OpenSSL | Native HTTP(S) for media; Curl for M3U/EPG/add-ons |
| Input | `xbmc/platform/ps4/PS4Input.*` | DualShock 4 |
| Python | embedded CPython 3.14 | Python add-ons, services and repository support |

## Video path

- Videodec2 outputs NV12/YV12 surfaces. The renderer aligns the decoder
  width to 64 pixels and packs the frame into a 896-pixel-wide FBO, then
  crops to the source width for the final pass.
- The image path uses the base YUV2RGB GLSL shader for both BT.709 and
  BT.601 sources. The source YUV matrix is used directly; the separate
  primaries-conversion program is disabled for the SD case.
- The shader sources are translated offline to PSSL/GNM blobs by
  `psbc`; the runtime hashes the exact define set and selects the blob.
- The GLES shim uses three frame buffers for the asynchronous present
  pipeline, waits for the previous flip and keeps one flip per vsync.

## A/V sync

- At start, the clock is anchored to the audio actually queued in the
  sink, not to the theoretical ActiveAE maximum. This removes the previous
  ~950 ms startup offset.
- Live streams use audio-master clock feedback. The audio is not
  speed-adjusted by the resampler; the video clock follows the audio with
  small `CDVDClock::ErrorAdjust` steps.
- A rebuffer watchdog checks the presented video PTS against the clock.
  When the video falls more than 2 s behind, it flushes audio/video and
  re-runs the start sync. A 30 s cooldown prevents flush loops.

## Audio

- `CAESinkPS4` exposes a 48 kHz stereo sink over `sceAudioOut`, in
  512-frame chunks. ActiveAE converts the decoder format and resamples
  44.1 kHz sources for the hardware.
- The sink reports its in-flight audio to ActiveAE and uses the system
  clock for the remaining pipeline.

## Network

- FFmpeg uses the native `libSceHttp`/`libSceSsl` transport for HTTP(S)
  media, including HLS.
- Curl keeps OpenSSL for M3U, EPG, artwork and add-on traffic.
- `pvr.iptvsimple` is linked into the executable because the PS4 SDK
  `dlopen()` is a stub.

## Memory and add-ons

- The port keeps a flexible memory reserve and temporary image/add-on
  guards so the GUI and Python can start under pressure. These guards are
  safety nets, not the target behavior; the 395-channel workload is the
  gate for removing them.
- CPython is cross-compiled for Orbis with the standard library shipped in
  the PKG. Binary add-ons must be built for Orbis; Python add-ons run from
  the embedded interpreter.

## HDR

The swapchain is 8-bit sRGB today. HDR10 is only announced when the
display, a 10-bit swapchain and BT.2020/PQ metadata are all available;
otherwise Kodi tone-maps HDR content to SDR.
