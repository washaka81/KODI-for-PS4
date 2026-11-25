# PS4 scene research — porting Kodi to PS4 (native homebrew)

> **Historical note (2026-10-16).** This document records the initial
> feasibility research. The hardware decode, native HTTP/TLS, Python and
> A/V sync work has since been implemented; see `docs/STATUS.md` and
> `docs/ARCHITECTURE.md` for the current state.


**Status:** research document, living draft. Last updated: 2026-10-16.
**Goal:** determine whether a native PS4 homebrew build of Kodi is realistic, what piece it requires,
and a staged implementation plan instead of a feasibility guess.

## 0. Verdict in one paragraph

Native userland PS4 homebrew is real in 2026 (Jailbreak + HEN + OpenOrbis toolchain up to FW 13.52),
the OS is FreeBSD 9 (x86_64), and the GPU is reachable through Piglet (OpenGL ES 2.x) plus GNM.
That means a Kodi **port is technically possible**, but it is a **large middleware rewrite**, not a build.
The realistic route is **Kodi's "platform" + audiosink + videosink + render backend + input backend** layered on
existing OpenOrbis/PS4 SDK symbols, with **software FFmpeg decode** (hardware Videodec2 only when the binding
is reversed) and GLES2 frame rendering. Recommendation: build a thin "Kodi-for-PS4" appliance milestone first
(M0–M2), and treat hardware video decode + addon python as Phase 2. A pure Linux route (PS4 Linux +
existing Kodi x86_64) remains the cheaper fallback and should be documented, not dismissed.

## 1. Scene state (2026)

| Topic | Facts | Source |
|---|---|---|
| OS / arch | FreeBSD 9 derived, 64-bit x86_64 | PS4 Developer Wiki |
| Jailbreak range | 5.05 → 13.52 with Kernel Exploit + HEN. "Golden" FWs: 5.05 / 6.72 / 9.00 / 11.00 / 12.02 / 13.52 | GBAtemp / ConsoleMods |
| Latest public root | etaHEN / GoldHEN layers on top of kernel exploit; higher firmwares had temporary webkit-only signs in 2026 trial builds | ConsoleModding / PSXHAX |
| SDK (free) | **OpenOrbis PS4 Toolchain** (clang + musl/libc + linker + stub libs) | OpenOrbis |
| Libraries | **orbisdev/liborbis**, SDL2 port (psxdev), OpenOrbis `oosdk_libraries` | GitHub |
| Packaging | **LibOrbisPkg** + `.fPKG`; distribution via PKG-Zone / HB-Store | OpenOrbis / GameBrew |
| Graphics | Piglet `scePigletSetConfigurationVSH` exposes **GLES2 + EGL**; also direct GNM via OpenGNM | OpenOrbis VSH header |
| Render option | `PS4-OpenGNM/vulkan-ps4` gives Vulkan 1.0 ICD over GNM | GitHub |

**Distribution rule:** homebrew ships as fPKG signed with ps4 fakesign; user installs from "Debug Settings" PKG or
remote package installers; app must request the standard entitlements and use a fixed title ID.

## 2. Hardware map relevant to media

- CPU: 8x AMD Jaguar @ ~1.6/2.13 GHz — fine for UI + audio control, *inadequate* for software 4K decode.
- GPU: AMD Radeon "Liverpool" GCN — only path that makes 4K plausible; must be accessed via GNM or GLES2 (Piglet)
  which internally drives GNM/PM4.
- VDEC blocks: `sceVideodec` (legacy) and `sceVideodec2CreateHevcDecoder` exist as symbols — list is known, runtime
  binding/reversal is **not** documented in public SDK headers (stub-only prototypes). Treat full hardware decode as
  Phase 2 with real reverse-engineering work.
- Audio out: `sceAudioOutOpen` with PCM (HDMI 5.1/7.1 or 2.0) documented in header stubs. Bitstream/HDMI output
  is not exposed cleanly; plan for **PCM**.
- Video out: `sceVideoOutOpen` + buffer management. Piglet manages its swap chain if we use GLES2.
- Input: `scePad` (DS4/DS5), and optional USB keyboard/mouse via kernel.

## 3. OpenOrbis SDK surface we can actually rely on

From `OpenOrbis/OpenOrbis-PS4-Toolchain/include/orbis`:

| Header | Usable for Kodi as |
|---|---|
| `AudioOut.h` | Audio sink backend (PCM) |
| `Audiodec*.h` | Optional audio decode research (unknown bindings) |
| `Videodec.h`, `Videodec2.h` | The only hardware decode surface; caller must reverse calls |
| `VideoOut.h`, `DbgVideoOut.h` | Display bring-up if we bypass Piglet |
| `Pigletv2VSH.h` | **Chosen GPU path**: EGL + GLES2 |
| `GnmDriver.h` | Advanced GNM (only if we write a renderer ourselves) |
| `UserService.h` | Account/user context for sce services |
| `SystemService.h` | App lifecycle, foreground/background, power |
| `Pad.h` | DS4/DS5 input driver |
| `Network` stubs | Sockets + HTTP via libc/OpenSSL ports |

Practical cross-build setup:

```sh
export OO_PS4_TOOLCHAIN=/opt/ps4/sdk            # or /opt/ps4/oosdk
export PS4SDK=$OO_PS4_TOOLCHAIN
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/ps4sdk.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Link list (from SDL-PS4 CMake, representative):

```
SceSystemService_stub_weak SceUserService_stub_weak ScePosix_stub_weak
ScePad_stub_weak SceAudioOut_stub_weak SceSysmodule_stub_weak
ScePigletv2VSH_stub_weak SceGnmDriver_stub_weak SceVideoOut_stub_weak
SceVideodec2_stub_weak libc_stub_weak.a
```

## 4. Kodi: requirements and where the work lands

Kodi at runtime needs these subsystems, and no other platform already accounts for PS4:

1. **Build system (CMake)** — add `PLATFORM=ps4` and toolchain file (existing model windows/linux/android).
2. **OS/network abstractions** — mostly POSIX; PS4/musl variants are minor.
3. **Logging/filesystem** — PS4 sandbox uses `/data/<titleid>/` writable; needs testing of `libc` calls.
4. **UI render backend** — easiest sane path: use Kodi's **GLES2 render backend** through Piglet EGL.
   Direct GNM renderer would be faster but costs months.
5. **Video playback pipeline** — Kodi uses FFmpeg. Two options:
   - *Phase 1:* software FFmpeg decoders + GLES2 YUV->RGB shader. Limit to 720p/1080p30.
   - *Phase 2:* optionally route decoder through `sceVideodec2*` for H.264/HEVC, with OS-backed DMA-BUF-like surfaces feeding GLES2.
6. **Audio sink** — new `CAESinkPS4` outputting PCM via `sceAudioOutOpen`/`sceAudioOutOutput`.
7. **Input** — `CPeripheral` extension polling `scePad` + USB keyboard.
8. **Remote UI control** — none in Phase 1; DS4 should map play/pause/seek.
9. **Python scripting (used by many addons)** — Kodi embeds CPython. `_types/Python` ports exist, but this is
   a *high-risk* multiplier: either build CPython for PS4 musl/clang or gate addon support behind Phase 3.
10. **HTTP/filesystem/network share plugins** — use libc/OpenSSL ports, pure userland.

**Compatibility assumption:** use a frozen modern Kodi release (e.g. current Omega/Piers-compatible snapshot) and
carry out the port against one baseline for at least 6 months to avoid upstream drift.

## 5. Milestones

- **M0 — Bring-up (2-3 weeks)**
  - OpenOrbis toolchain builds, env vars, empty app linking stubs.
  - Renders textured triangle on PS4 via Piglet GLES2 + EG L and exits on DS4.
- **M1 — Kodi compiles on PS4 (1-2 months)**
  - Platform stub created; configure against frozen Kodi source; fix compile errors (system libs, threading, dynamic loading).
  - No playback yet; UI boots to home.
- **M2 — Media appliance (1-2 months)**
  - Filesystem + network shares; software H.264/H.265 playback of local media; PCM audio; DS4 navigation.
  - Milestone where the app becomes useful.
- **M3 — Performance (2+ months, parallelizable)**
  - PS4 VDEC2 integration if symbols/bindings are reversed; 1080p60/4K target.
  - Cache, memory budgets (piglet shared system memory caps 0x20000000).
- **M4 — Addons / Python (optional)**
  - Embed CPython; enable Python-based addons; skins and scrapers.
- **M5 — Release packaging + store distribution**
  - fPKG signing, title ID, assets, icon, HB-Store submission.

**Out of scope initially:** PVR live TV addons, HDR, Dolby Atmos bitstream, skin engine rework, DRM/commercial streams,
video passthrough, CEC, cross-platform multi-user accounts, remote play streaming.

## 6. Risks and unknowns (ranked)

1. **Hardware decoder bindings unknown** — public headers are empty stubs. Falls back to software decode indefinitely.
2. **Python cross-build for PS4** — CPython ABI + musl + clang. Could block addons entirely.
3. **GLES2 driver limits** — Piglet memory caps and threading quirks are poorly documented; only way is testing.
4. **Kodi upstream size** — one mid-size PR per month already struggles; a full platform needs dedicated maintenance.
5. **Libx** dynamic loading — Kodi uses `dlopen`; PS4 userland has libdl, but sandbox may disable. Validate early.
6. **Firmware churn** — scene offers highest stable root, but targetting latest 13.52 limits reverse-engineering
   accuracy; many homebrews preferred 9.00/11.00 for stability.
7. **Legal/publishing** — fPKG is fake-signed; cannot sell or use Sony trademarks; sandbox prohibits DRM assets.

## 7. Recommended setup & scaffold

Create this repository layout (target when implementation starts):

```
kodi-ps4/
  CMakeLists.txt             # orchestrates toolchain + source submodule
  cmake/ps4sdk.cmake        # OpenOrbis toolchain integration
  cmake/toolchain.cmake     # renamed from ps4sdk with SDK detection
  src/
    platform/ps4/
      PS4Platform.cpp
      PS4RendererGLES2.*
      PS4AudioSink.*
      PS4Input.*
    kodi/                   # submodule pinned to frozen Kodi release
  packaging/
    sfo.xml                 # title ID, category
    icon0.png
  scripts/build.sh
  docs/                     # this file
```

Initial build checklist:

- [ ] Install OpenOrbis toolchain on Linux host and verify `clang` target `x86_64-scei-ps4`.
- [ ] Compile a minimal Piglet GLES2 sample; submit to test console.
- [ ] Create new Kodi `platform/ps4` CMake with KHR includes.
- [ ] Define `CAESinkPS4`, `CEGLPS4`, `CPadPS4`, `CFile` path abstraction.
- [ ] Smoke-test `dlopen`, C++ exceptions, threads, and filesystem write for one app sandbox.
- [ ] Lock baseline Kodi release tag in repo.

## 8. Alternative routes (cost/benefit)

| Route | Pros | Cons |
|---|---|---|
| Native homebrew (this doc) | Full performance, excellent UI, native gamepad/PiGlet | Huge engineering load, unknown VDEC |
| PS4 Linux (PS4 Linux + distro Kodi) | Existing Kodi build works, no SDK port | Requires Linux jailbreak payload, no native game UI, worse startup, risk per FW |
| Full GNM (no Piglet) | Max performance, no shared memory cap surprises | Month-long renderer, reinvent shaders |
| Soft-pkg libs in userspace EEL | Already proves media player possible | Older, abandonware, bad ergonomics |

## 9. Quick-start acceptance tests for any port attempt

Each must pass on a real PS4 (5.05 or 11.00 dev console) before declaring progress:

1. `sceSysmoduleLoadModule` + `sceUserServiceInitialize` return `SCE_OK`.
2. Piglet creates 1080p swap chain with DS4 rendering a textured quad.
3. PCM sine wave audible at 48 kHz via `sceAudioOutOutput`.
4. Read/write in app data folder works and survives reboot.
5. FFmpeg can decode 1080p H.264 software to buffer (with FPS telemetry in debug log).

## 10. References (selected)

- PS4 Developer Wiki: System Software, Open Orbis SDK, Media Services: https://www.psdevwiki.com/
- ConsoleMods: PS4 scene history, FW exploit chart, standard jailbreak: https://consolemods.org/wiki/PS4
- OpenOrbis Toolchain: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
- OpenOrbis liborbis / oosdk_libraries / SDL-PS4: https://github.com/orbisdev/liborbis https://github.com/OpenOrbis/oosdk_libraries https://github.com/OpenOrbis/SDL-PS4
- OpenOrbis OpenGNM / vulkan-ps4: https://github.com/PS4-OpenGNM/vulkan-ps4
- Kodi official docs / supported hardware: https://kodi.wiki/view/Supported_hardware
- PS4 Linux (historical psxitarch + Kodi guide): PSXHAX / PSX-Place

## 11. Source references and how to use them

### 11.1 xbmc/xbmc (Kodi upstream)

Path anchors to copy design from, not fork wholesale:

- **Platform base:** `xbmc/platform/freebsd` + `xbmc/platform/posix`
  - reuse the FreeBSD POSIX plumbing directly; rename to `ps4` where needed.
  - `PlatformFreebsd.*`, `CPUInfoFreebsd.*`, `GPUInfoFreebsd.*`, `MemUtils.*`.
- **Render:** `xbmc/rendering/gles` + `RenderSystem*.cpp`
  - primary GPU path through Piglet GLES2; avoids writing GNM shaders.
  - OpenGL ES extensions and EGL integration already exist.
- **Audio sinks:** `xbmc/cores/AudioEngine/Sinks/AESinkOSS.*` and `AESinkALSA.*`
  - pattern for thread + buffer + PCM port; use as template for `CAESinkPS4`.
- **Input:** `xbmc/input` (peripheral abstraction) and Linux joypad udev-like readers.
- **Build:** `CMakeLists.txt` + `cmake/` modules; platform selection switches.
  - add `PLATFORM=ps4` and toolchain integration as a new branch.

**Porting rule:** pin the exact Kodi tag, add the platform, and upstream only optional, small, non-platform PRs.

### 11.2 DeathRGH/frame4

Frame4 is **not a media player**; it is a PS4 debugger (`ps4debug` derivative) plus `ps4-payload-sdk` and
kernel-side tooling. Its value to this project is bring-up and diagnostics:

- **`ps4-payload-sdk/`** for low-level PS4 syscall ABI and link examples.
- **`installer/crt0.s`** for minimal ELF startup details.
- **`kdebugger/`** for attaching to homebrew app during performance/debug runs (when porting runtime).
- **`ps4-ksdk/source/ksdk.c`** reference for kernel-friendly memory and process hooks.

**Do not depend on frame4 runtime** in the media app; use OpenOrbis toolchain for production and reserve frame4
for debugging/performance analysis on test consoles.

### 11.3 Decompilation / reverse engineering guidance

- "Decompiling" is only necessary for `sceVideodec2*` real bindings and Piglet resource setup; otherwise the public
  stub headers plus frame4's SDK examples should be enough for the GLES2 milestone.
- Keep reverse-engineering work isolated in `src/platform/ps4/reversing/` with clear provenance; do not ship
  proprietary Sony blobs.
- Document every symbol used from unreleased headers with source and FW version tested.

### 11.4 morluto/rea (reverse engineering tool)

REA is a **reverse-engineering workspace** ("reverse engineer anything with agents, from app behavior
down to native binaries"). In this project it is relevant as an **analysis aid**, not as a runtime dependency:

- Use it to annotate and reason about **PS4 ELF/SPRX** flows when documenting `sceVideodec2*` real
  bindings or Piglet resource setup.
- Prefer static symbol discovery plus documented FW versions; never ship proprietary Sony binaries.
- Keep REA findings under `docs/reversing/` with provenance (binary hash, FW, tool version).
- Do not confuse REA with a PS4 toolchain; production builds remain on OpenOrbis tooling.

## 12. Lab device discovered on LAN

- Console hostname: `PS4-4340BE.local`
- IP: `192.0.2.12` (stable at least during this session)
- Wi-Fi MAC: `<console-mac>` (AzureWave; consistent with PS4 Wi-Fi module)
- Homebrew layer: GoldHEN installed under `/data/GoldHEN`
- Access methods now validated:
  - **FTP (anonymous, root-visible):** `ftp://192.0.2.12:2121/` (full root FS view)
  - **9090/tcp:** open (payload/binloader endpoint, commonly used by exploit tools)
- Closed/filtered: 21/tcp, 22/tcp (no native SSH), and other payload ports when idle.
- Use FTP for deploying test builds and collecting logs; reserve 9090 for payload injection if needed.

Operational rules:
1. Never write/delete outside `user/`, `data/`, or obvious temp paths without a backup.
2. Keep `.pkg` test installs in a dedicated naming scheme (`kodi-ps4-<ver>.pkg`) and retain previous version.
3. Inspect `sce_logs/` and `Loader_Logs.txt` before/after crashes.
4. Treat console as an appliance; no risky memory writes during M0 bring-up.

## 13. First M0 build and install pipeline (verified)

- Toolchain used: OpenOrbis `v0.5.4` at `$HOME/ps4sdk/OpenOrbis/PS4Toolchain`
- Commands:
  - `./scripts/build.sh`
  - `./scripts/package.sh`
- Output:
  - `build/kodi-ps4.elf`
  - `build/eboot.bin`
  - `dist/IV0001-CUSA54261_00-KODIPS4000000000.pkg`
- Console target:
  - `ftp://192.0.2.12:2121/data/pkg/IV0001-CUSA54261_00-KODIPS4000000000.pkg` (uploaded OK)
- Next manual action on PS4: install the uploaded PKG through GoldHEN Package Installer, then boot and collect:
  - XMB launch result
  - `/data/sce_logs`
  - `/data/Loader_Logs.txt`
