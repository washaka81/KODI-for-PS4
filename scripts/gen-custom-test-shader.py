#!/usr/bin/env python3
"""Build the diagnostic full-screen shader used by PS4_DrawTestShader().

The vertex/fragment sources must stay byte-identical to the constants in
src/kodi/xbmc/platform/ps4/PS4ShaderBin.cpp: the runtime hashes the source
with djb2 and looks up system/shaders/bin/<hash>.bin.

Usage:
    ./scripts/gen-custom-test-shader.py [--out build/shaderbin]
"""
import argparse
import importlib.util
import pathlib
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
VS = (b"#version 100\nattribute vec2 a_pos;\n"
      b"void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n")
FS = (b"#version 100\nprecision mediump float;\n"
      b"void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n")


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "gen_shader_bins", ROOT / "scripts" / "gen-shader-bins.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "build" / "shaderbin"))
    ap.add_argument("--psbc", default="/tmp/kodi-ps4/psbc/psbc")
    args = ap.parse_args()

    g = load_generator()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for stage, source in (("vertex", VS), ("fragment", FS)):
        translated = g.translate_es100_to_es310(
            stage, source, attr_location={"a_pos": 0}, varying_location={})
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            glsl = tmp / "custom.glsl"
            spv = tmp / "custom.spv"
            sb = tmp / "custom.sb"
            glsl.write_bytes(translated)
            g.run(["glslangValidator", "-V", "-R",
                   "-S", "vert" if stage == "vertex" else "frag",
                   "-o", str(spv), str(glsl)])
            g.run([args.psbc, "-s", stage, "-f", str(spv), "-o", str(sb)])
            psb = g.build_param_tables(
                source, stage, spv.read_bytes(), sb.read_bytes(),
                varying_locations={})
            blob = g.create_blob(source, psb)
            digest = g.djb2(source)
            (out / f"{digest:08x}.bin").write_bytes(blob)
            print(f"{stage}: {digest:08x}.bin ({len(blob)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
