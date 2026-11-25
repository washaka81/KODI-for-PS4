#!/usr/bin/env python3
"""Generate the PS4 Piglet shader binaries used by the video renderer.

gen-shader-bins.py only covers the GUI shaders.  The YUV2RGB shaders are
compiled at playback time with a format define plus the optional
colour-conversion, tone-mapping and dither defines, so they need their own
blobs.  This tool reuses the same offline pipeline and writes <djb2>.bin
files into build/shaderbin.

The runtime hash includes every define.  A missing combination (for example
XBMC_PS4_PACKED + XBMC_COL_CONVERSION for an SD BT.601 channel) makes the
whole shader fail to compile and, on the current retry path, can exhaust the
PS4 program table.  Generate the complete matrix instead of only the base
variants.
"""

import argparse
import importlib.util
import os
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Runtime order matches BaseYUV2RGBGLSLShader:
#   format, XBMC_COL_CONVERSION, tone-map, XBMC_DITHER, PS4_SRGB_SWAPCHAIN
FORMAT_DEFINES = [
    ("XBMC_YV12", "#define XBMC_YV12\n"),
    ("XBMC_YV12_HI", "#define XBMC_YV12_HI\n"),
    ("XBMC_NV12", "#define XBMC_NV12\n"),
    ("XBMC_NV12_RRG", "#define XBMC_NV12_RRG\n"),
    ("XBMC_YUY2", "#define XBMC_YUY2\n"),
    ("XBMC_UYVY", "#define XBMC_UYVY\n"),
    ("XBMC_Y210", "#define XBMC_Y210\n"),
    ("XBMC_AYUV", "#define XBMC_AYUV\n"),
    ("XBMC_Y410", "#define XBMC_Y410\n"),
    ("XBMC_Y412", "#define XBMC_Y412\n"),
    ("XBMC_PS4_PACKED", "#define XBMC_PS4_PACKED\n"),
]

COLOR_CONVERSION_DEFINES = [
    ("", ""),
    ("colconv", "#define XBMC_COL_CONVERSION\n"),
]

DITHER_DEFINES = [
    ("", ""),
    ("dither", "#define XBMC_DITHER\n"),
]

TONEMAP_DEFINES = [
    ("", ""),
    ("reinhard", "#define KODI_TONE_MAPPING_REINHARD\n"),
    ("aces", "#define KODI_TONE_MAPPING_ACES\n"),
    ("hable", "#define KODI_TONE_MAPPING_HABLE\n"),
]

# Kodi appends these snippets to the YUV2RGB fragment shaders with
# CShader::InsertSource() before hashing the final source.
FRAGMENT_INSERTS = [
    ("gles_tonemap.frag", "void main()"),
    ("gles_dither_uniforms.frag", "void main()"),
    ("gles_dither_body.frag", "gl_FragColor"),
]

STABLE_FRAGMENT_NAMES = {
    "XBMC_YV12": ("yv12_basic.bin", "yv12_bob.bin"),
    "XBMC_NV12": ("nv12_basic.bin", "nv12_bob.bin"),
    "XBMC_PS4_PACKED": ("packed_basic.bin", "packed_bob.bin"),
}


def load_generator():
    path = ROOT / "scripts" / "gen-shader-bins.py"
    spec = importlib.util.spec_from_file_location("gen_shader_bins", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def insert_file(source: bytes, shaders_dir: str, name: str, location: str) -> bytes:
    snippet = open(os.path.join(shaders_dir, name), "rb").read()
    pos = source.find(location.encode())
    if pos < 0:
        raise RuntimeError(f"{name}: cannot find {location!r}")
    return source[:pos] + snippet + source[pos:]


def final_source(shaders_dir: str, stage: str, fname: str, defines: str) -> bytes:
    """Build the exact source the runtime hashes for one shader stage."""
    g = load_generator()
    final = g.insert_prefix(open(os.path.join(shaders_dir, fname), "rb").read(), defines)
    if stage == "fragment":
        for snippet, location in FRAGMENT_INSERTS:
            final = insert_file(final, shaders_dir, snippet, location)
    return final


def variant_defines(fmt: str, colconv: str, tonemap: str, dither: str) -> str:
    """Same define order as BaseYUV2RGBGLSLShader's m_defines."""
    return (fmt + colconv + tonemap + dither + "#define PS4_SRGB_SWAPCHAIN\n")


def fragment_variants(shaders_dir: str):
    """Yield every fragment combination the renderer can request."""
    for label, fmt in FORMAT_DEFINES:
        for cc_label, cc in COLOR_CONVERSION_DEFINES:
            for tm_label, tm in TONEMAP_DEFINES:
                for d_label, dither in DITHER_DEFINES:
                    defines = variant_defines(fmt, cc, tm, dither)
                    tag = "_".join(
                        part for part in (label, cc_label, tm_label, d_label) if part)
                    is_base = not (cc_label or tm_label or d_label)
                    for frag in ("gles_yuv2rgb_basic.frag", "gles_yuv2rgb_bob.frag"):
                        yield tag, frag, defines, is_base


def compile_one(g, args, shaders_dir: str, stage: str, fname: str, final: bytes,
                extra=()):
    """Translate, compile and package one shader blob."""
    vert_path = os.path.join(shaders_dir, "gles_yuv2rgb.vert")
    attrs, vs_varyings = g.extract_io(open(vert_path, "rb").read())
    if stage == "vertex":
        varyings = vs_varyings
        attr_location = {n: i for i, n in enumerate(attrs)}
    else:
        _, fs_varyings = g.extract_io(open(os.path.join(shaders_dir, fname), "rb").read())
        varyings = fs_varyings
        missing = [n for n in fs_varyings if n not in vs_varyings]
        if missing:
            raise RuntimeError(
                f"{fname}: varyings not declared by gles_yuv2rgb.vert: {missing}")
        attr_location = None
    varying_location = {n: i for i, n in enumerate(vs_varyings)}

    spvsrc = g.translate_es100_to_es310(
        stage, final, attr_location, varying_location, push_constants=True
    )
    with tempfile.TemporaryDirectory() as tmp:
        srcpath = os.path.join(tmp, "cur.glsl")
        with open(srcpath, "wb") as fh:
            fh.write(spvsrc)
        spv = os.path.join(tmp, "cur.spv")
        sb = os.path.join(tmp, "cur.sb")
        g.run(["glslangValidator", "-V", "-R", "-S",
               "vert" if stage == "vertex" else "frag", "-o", spv, srcpath])
        g.run([args.psbc, *extra, "-s", stage, "-f", spv, "-o", sb])
        psb = g.build_param_tables(
            final, stage, open(spv, "rb").read(), open(sb, "rb").read(),
            include_attrs=True, include_uniforms=True, include_samplers=True,
            varying_locations=varying_location,
        )
    return g.djb2(final), g.create_blob(final, psb)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shaders-dir", default=str(ROOT / "src/kodi/system/shaders/GLES/2.0"))
    ap.add_argument("--psbc", default=str(Path.home() / "kodi-build-deps/psbc/psbc"))
    ap.add_argument("--out", default=str(ROOT / "build/shaderbin"))
    args = ap.parse_args()

    g = load_generator()
    os.makedirs(args.out, exist_ok=True)

    # The vertex program is identical for every define combination: the
    # defines only affect the fragment stage.  One stable blob avoids
    # generating ~350 copies and, more importantly, removes the vertex hash
    # from the failure surface.  PS4ShaderBin maps every YUV vertex source
    # to this file.
    vert_defines = variant_defines("#define XBMC_PS4_PACKED\n", "", "", "")
    vert_final = final_source(args.shaders_dir, "vertex", "gles_yuv2rgb.vert", vert_defines)
    vert_hash, vert_blob = compile_one(
        g, args, args.shaders_dir, "vertex", "gles_yuv2rgb.vert", vert_final)
    with open(os.path.join(args.out, f"{vert_hash:08x}.bin"), "wb") as fh:
        fh.write(vert_blob)
    shutil.copyfile(
        os.path.join(args.out, f"{vert_hash:08x}.bin"),
        os.path.join(args.out, "yuv2rgb_vert.bin"),
    )
    print(f"gles_yuv2rgb.vert: stable yuv2rgb_vert.bin ({vert_hash:08x})")

    total = 1
    for tag, frag, defines, is_base in fragment_variants(args.shaders_dir):
        final = final_source(args.shaders_dir, "fragment", frag, defines)
        # psbc 0.0.1 hangs the GPU on the uniform-exponent pow sequence in
        # the XBMC_COL_CONVERSION variants; -Od is the diagnostic workaround.
        extra = ("-Od",) if "colconv" in tag else ()
        hash_, blob = compile_one(g, args, args.shaders_dir, "fragment", frag, final,
                                  extra)
        outpath = os.path.join(args.out, f"{hash_:08x}.bin")
        with open(outpath, "wb") as fh:
            fh.write(blob)
        if is_base:
            fmt_label = tag
            stable = STABLE_FRAGMENT_NAMES.get(fmt_label)
            if stable:
                name = stable[1] if "bob" in frag else stable[0]
                shutil.copyfile(outpath, os.path.join(args.out, name))
                print(f"{frag}: stable {name}")
        print(f"{frag}: {tag or 'base'} {hash_:08x} -> {outpath}")
        total += 1

    print(f"generated {total} video shader blobs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
