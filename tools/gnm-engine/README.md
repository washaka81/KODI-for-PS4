# gnm-engine — GPU render engine for the Kodi PS4 port

A small render engine built directly on **sceGnm + sceVideoOut** (no
Piglet/OpenGL). It is the test bench for the pipeline that is later linked
into Kodi through the GLES/EGL shim.

## Status

Working on hardware: a 625×625 textured quad with the Kodi splash, centred
and with the correct aspect ratio, presented through VideoOut. Per frame:
clear, VS/PS `.sb` binaries (compiled offline with `glslc` + `psbc`),
sampler + linear RGBA8 sRGB texture, VBO with position/UV, `DrawIndexAuto(6)`,
flip.

Pipeline pieces already validated:

- `displayctx` (sceVideoOut): 1920×1080, one render target, one flip per frame.
- Linear 2D texture uploaded from an in-memory PNG (pitch = padded width).
- Descriptor sets through `gnmDrawCmdSetPointerUserData` (VS: fetch + VBOs;
  PS: texture + sampler).
- Fetch shader created by `gnmCreateFetchShader` from the `.sb` semantics.
- Synchronization with `EventWriteEop` + label and `sceGnmSubmitDone`.

## Build

Requirements: an OpenOrbis toolchain and the `opengnm`/`psbc` dependencies
created by `scripts/setup-psbc.sh` (default work dir `/tmp/kodi-ps4`).

```bash
export OO_PS4_TOOLCHAIN=/opt/ps4/sdk
export PSBC_BASE=/tmp/kodi-ps4
./scripts/setup-psbc.sh

cd tools/gnm-engine
make GLSLC=glslc PSBC=/tmp/kodi-ps4/psbc/psbc \
     RUNTIME_MODULE_DIR="$OO_PS4_TOOLCHAIN/samples/graphics/sce_module"
```

The build generates the `.spv`/`.sb` shader binaries and the test package.
The Sony runtime modules (`libc.prx`, `libSceFios2.prx`) are copied from the
toolchain samples at build time and are not part of this repository.

## Known pitfalls

1. **`gnmCreateVertexBuffer` receives a VERTEX count**, not bytes
   (`numelements`). Passing `sizeof(vertices)` corrupts the fetch and hangs
   the GPU.
2. `assets/misc/clear.frag.sb` must be packaged: `initclearutility` loads it
   at startup; without it the app exits before creating the window.
3. Linear textures need a **pitch multiple of 64/8**: the PNG is padded to
   640×632 and the UVs are clipped to the real 625×625 area.
4. `PkgTool.Core` needs `DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1`;
   `pkg_build` needs OpenSSL 1.1 (see `scripts/package-core.sh`).
5. The runtime modules are under
   `$OO_PS4_TOOLCHAIN/samples/graphics/sce_module`, not under
   `$OO_PS4_TOOLCHAIN/bin/data/modules`.

## GLES shim

`glshim/` implements the GLES2/EGL subset Kodi needs on top of this engine:
`glClear`, `glDrawElements`, `glBindTexture`, `glUniform*`, shaders,
blend/scissor, VBOs and the present/flip path. Kodi links against
`libglshim.a`; there is no Piglet dependency.
