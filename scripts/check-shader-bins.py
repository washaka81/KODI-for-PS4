#!/usr/bin/env python3
"""Check that build/shaderbin matches the current GLES shader sources.

The GUI runtime hashes the exact ES 1.00 source it compiles; a stale
build/shaderbin (for example, one generated before a Kodi base update)
silently produces `Invalid GUI Shader selected` and a black GUI.  This
check fails the package step instead of shipping stale blobs.

It intentionally skips the `_oes` shader variants: the PS4 GLES shim does
not expose `GL_OES_EGL_image_external`, so their generation is allowed to
fail and they are not used by the current renderer.
"""

import argparse
import importlib.util
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def load_generator():
    path = ROOT / "scripts" / "gen-shader-bins.py"
    spec = importlib.util.spec_from_file_location("gen_shader_bins", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shaders-dir", default=str(ROOT / "src/kodi/system/shaders/GLES/2.0"))
    ap.add_argument("--out", default=str(ROOT / "build/shaderbin"))
    args = ap.parse_args()

    g = load_generator()
    out = pathlib.Path(args.out)
    shaders = pathlib.Path(args.shaders_dir)
    missing = []

    for vert, frag in g.GUI_SHADERS:
        if "_oes" in vert or "_oes" in frag:
            continue
        for defines in g.DEFINES_VARIANTS:
            for fname in (vert, frag):
                final = g.insert_prefix((shaders / fname).read_bytes(), defines)
                hash_ = f"{g.djb2(final):08x}"
                if not (out / f"{hash_}.bin").exists():
                    missing.append(f"{fname} {hash_}")

    for stable in ("yv12_basic.bin", "yv12_bob.bin", "nv12_basic.bin",
                   "nv12_bob.bin", "packed_basic.bin", "packed_bob.bin",
                   "yuv2rgb_vert.bin"):
        if not (out / stable).exists():
            missing.append(stable)

    # Verify every YUV fragment combination the renderer can request.  A
    # missing colour-conversion variant is not cosmetic: the shader fails to
    # compile and the retry loop can exhaust the PS4 program table.
    path = ROOT / "scripts" / "gen-video-shader-bins.py"
    spec = importlib.util.spec_from_file_location("gen_video_shader_bins", path)
    vgen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vgen)

    # The stable vertex blob embeds the exact source it was built from.
    # Comparing it catches a stale blob after a gles_yuv2rgb.vert change.
    vert_defines = vgen.variant_defines("#define XBMC_PS4_PACKED\n", "", "", "")
    vert_final = vgen.final_source(str(shaders), "vertex", "gles_yuv2rgb.vert", vert_defines)
    vert_blob = out / "yuv2rgb_vert.bin"
    if vert_blob.exists():
        raw = vert_blob.read_bytes()
        if len(raw) < 20:
            missing.append("yuv2rgb_vert.bin truncated")
        else:
            src_len = struct.unpack_from("<I", raw, 16)[0]
            if raw[20:20 + src_len] != vert_final:
                missing.append("yuv2rgb_vert.bin stale")

    for tag, frag, defines, is_base in vgen.fragment_variants(str(shaders)):
        final = vgen.final_source(str(shaders), "fragment", frag, defines)
        hash_ = f"{g.djb2(final):08x}"
        blob = out / f"{hash_}.bin"
        if not blob.exists():
            missing.append(f"{frag} {tag or 'base'} {hash_}")
            continue
        if is_base:
            stable = vgen.STABLE_FRAGMENT_NAMES.get(tag)
            if stable:
                stable_path = out / (stable[1] if "bob" in frag else stable[0])
                if not stable_path.exists():
                    missing.append(stable_path.name)
                elif stable_path.read_bytes() != blob.read_bytes():
                    missing.append(f"{stable_path.name} != {hash_}.bin")

    if missing:
        print("ERROR: build/shaderbin does not match the current shader sources:", file=sys.stderr)
        for item in missing:
            print(f"  missing {item}", file=sys.stderr)
        print("Regenerate with:", file=sys.stderr)
        print("  python3 scripts/gen-shader-bins.py --shaders-dir src/kodi/system/shaders/GLES/2.0 --out build/shaderbin", file=sys.stderr)
        print("  python3 scripts/gen-video-shader-bins.py --shaders-dir src/kodi/system/shaders/GLES/2.0 --out build/shaderbin", file=sys.stderr)
        return 1

    print(f"shaderbin OK: {out} matches the current GLES sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
