#!/usr/bin/env python3
"""Generate precompiled PS4 Piglet shader binaries for Kodi's GLES shaders.

Piglet has no runtime GLSL compiler, so every shader Kodi compiles with
glShaderSource/glCompileShader fails with an empty info log and the GUI stays
black. This tool compiles the GLSL ES 1.00 sources offline:

    GLSL ES 1.00 -> (mechanical ES 1.00 -> 3.10 translation)
                 -> SPIR-V (glslangValidator)
                 -> PS4 Shader Binary (psbc, bizkutMes NIR+ACO)
                 -> wrapped in Piglet's binary container
                 -> build/shaderbin/<djb2 hash of final ES source>.bin

Non-opaque uniforms are emitted as a Vulkan push-constant block so the
patched psbc (see tools/patches/psbc-kodi-ps4.patch) emits an
IMM_CONSTBUFFER usage slot.  That is the GNM equivalent of PSSL's
__GLOBAL_CB__, and it is where Piglet writes glUniform* data.  The PSSL
parameter table mirrors ShaccVSH: element names are the real uniform names,
the CB numelems is the number of variables, ATTR_/VAR_ semantic names are
kept distinct, and only interfaces that survived SPIR-V optimisation are
listed (declared-but-unused varyings have no GNM semantic).

The blob layout was recovered from 124 blobs captured on a PS4 by the
ioQuake3-PS4 port (Mayo1970) and holds for all of them:

    [u32 format 0x9270][u32 magic 0xE891BC71][u32 100][u32 1]
    [u32 sourceLength][source bytes][u32 psbLength][psb bytes]

where `psb` is the full psbc .sb output (PsslBinaryHeader + GnmShaderFileHeader
"Shdr" + stage data + GCN code + GnmShaderBinaryInfo trailer) and the source
is the exact final ES 1.00 source that Kodi hashes at runtime (djb2).

Kodi picks blobs up at runtime via PS4_LoadShaderBinary()."""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

VERSION = 100
FORMAT = 0x9270
MAGIC = 0xE891BC71

# (vert, frag) pairs as loaded by CRenderSystemGLES::InitialiseShaders and the
# composite shader; None vert means "gles_shader.vert".
GUI_SHADERS = [
    ("gles_shader.vert", "gles_shader_default.frag"),
    ("gles_shader.vert", "gles_shader_texture.frag"),
    ("gles_shader.vert", "gles_shader_texture_111r.frag"),
    ("gles_shader.vert", "gles_shader_multi.frag"),
    ("gles_shader.vert", "gles_shader_multi_rgba_111r.frag"),
    ("gles_shader_simple.vert", "gles_shader_fonts.frag"),
    ("gles_shader_clip.vert", "gles_shader_fonts.frag"),
    ("gles_shader.vert", "gles_shader_texture_noblend.frag"),
    ("gles_shader.vert", "gles_shader_multi_blendcolor.frag"),
    ("gles_shader.vert", "gles_shader_multi_rgba_111r_blendcolor.frag"),
    ("gles_shader.vert", "gles_shader_multi_111r_111r_blendcolor.frag"),
    ("gles_shader.vert", "gles_shader_rgba.frag"),
    ("gles_shader.vert", "gles_shader_rgba_blendcolor.frag"),
    ("gles_shader.vert", "gles_shader_rgba_bob.frag"),
    ("gles_shader.vert", "gles_shader_rgba_oes.frag"),
    ("gles_shader.vert", "gles_shader_rgba_bob_oes.frag"),
    ("gles_shader.vert", "gles_shader_texture_noalpha.frag"),
    ("gles_gui_composite.vert", "gles_gui_composite.frag"),
]

# Variants of the defines prefix that InitialiseShaders can build.
DEFINES_VARIANTS = [
    "",
    "#define KODI_LIMITED_RANGE 1\n",
    "#define KODI_TRANSFER_PQ 1\n",
    "#define KODI_LIMITED_RANGE 1\n#define KODI_TRANSFER_PQ 1\n",
]


def djb2(buf: bytes) -> int:
    h = 5381
    for c in buf:
        h = ((h * 33) & 0xFFFFFFFF) + c
    return h & 0xFFFFFFFF


def insert_prefix(source: bytes, prefix: str) -> bytes:
    """Replicates CShader::LoadSource: prefix is inserted after #version line."""
    if not prefix:
        return source
    pos = source.find(b"#version")
    if pos != -1:
        eol = source.find(b"\n", pos)
        if eol != -1:
            return source[: eol + 1] + prefix.encode() + source[eol + 1 :]
    return source


def extract_io(source: bytes):
    """Collect attribute and varying declarations from an ES 1.00 source."""
    import re

    attrs, varyings = [], []
    for m in re.finditer(r"^\s*(attribute|varying)\s+(\w+\s+)*(\w+)\s*;\s*$", source.decode(), re.M):
        (attrs if m.group(1) == "attribute" else varyings).append(m.group(3))
    return attrs, varyings


def translate_es100_to_es310(stage: str, source: bytes, attr_location=None, varying_location=None,
                              push_constants=True) -> bytes:
    """Mechanical ES 1.00 -> Vulkan-GLSL (3.10 es + UBO/samplers) translation.

   SPIR-V is produced from this translation; the blob always embeds the
    original ES 1.00 text, which is also what the runtime hash covers.
    """
    import re

    text = source.decode()
    pos = text.find("#version")
    if pos == -1:
        raise RuntimeError("shader has no #version line")
    eol = text.find("\n", pos)
    if eol == -1:
        raise RuntimeError("#version line not terminated")
    body = text[eol + 1 :]

    def swap_word(body: str, old: str, new: str) -> str:
        return re.sub(rf"\b{re.escape(old)}\b", new, body)

    # qualifier swap: attribute -> in; varying -> out (VS) / in (FS)
    body = swap_word(body, "attribute", "in")
    body = swap_word(body, "varying", "out" if stage == "vertex" else "in")

    # the ESSL1 EGL external image extension has an ESSL3 equivalent
    body = body.replace(
        "GL_OES_EGL_image_external :", "GL_OES_EGL_image_external_essl3 :", 1
    )
    body = body.replace(
        "GL_OES_EGL_image_external:", "GL_OES_EGL_image_external_essl3 :", 1
    )
    # SPIR-V needs explicit locations on user IO; locator maps keyed by name
    # make VS outputs and FS inputs agree.
    locators = dict(attr_location or {})
    locators.update(varying_location or {})

    def add_loc(m):
        ws, q, ty, nm = m.group(1), m.group(2), m.group(3), m.group(4)
        loc = locators.get(nm)
        if loc is None:
            return m.group(0)
        return f"{ws}layout(location = {loc}) {q} {ty} {nm};"

    body = re.sub(
        r"(?m)^(\s*)(in|out)\s+(\w+\s+)*(\w+)\s*;",
        lambda m: add_loc(m) if m.group(4) in locators else m.group(0),
        body,
    )

    # texture2D/textureCube only exist in legacy GLSL; map them to ES 3.0 names.
    body = swap_word(body, "texture2DLod", "textureLod")  # do lod before 2D swap
    body = swap_word(body, "texture2DProj", "textureProj")
    body = swap_word(body, "texture2D", "texture")
    body = swap_word(body, "textureCube", "texture")

    # Vulkan GLSL: non-opaque uniforms must come from a UBO; references keep
    # working unqualified when the block is anonymous (no instance name).
    block_members = []  # (name, decl) in declaration order

    def take_block(m):
        decl = re.sub(r"^\s*uniform\s+", "", m.group(0).strip()).rstrip(";")
        decl = re.sub(r"^(low|medi|high)p\s+", "", decl)
        block_members.append((m.group(2), decl))
        return "/*%s removed*/" % m.group(2)

    body = re.sub(
        r"^\s*uniform\s+(?!sampler)(\w+\s+)+(\w+)\s*;\s*(?://.*)?$", take_block, body, flags=re.M
    )

    # samplers need explicit set/binding to be valid Vulkan GLSL
    sampler_index = [0]

    def add_sampler_binding(m):
        decl = m.group(0).strip()
        out = decl.replace(
            "uniform ", f"layout(set = 0, binding = {1 + sampler_index[0]}) uniform ", 1
        )
        sampler_index[0] += 1
        return out

    body = re.sub(r"^\s*uniform\s+sampler\w*\s+\w+\s*;\s*(?://.*)?$", add_sampler_binding, body, flags=re.M)

    header = ["#version 310 es", "precision highp float;", "precision highp int;"]
    if stage != "vertex":
        body = re.sub(r"\bgl_FragColor\b", "m_fragColor", body)
        body = "layout(location = 0) out highp vec4 m_fragColor;\n" + body
    if block_members:
        # Piglet binds the PSSL __GLOBAL_CB__ through an IMM_CONSTBUFFER
        # usage slot.  Push constants make psbc emit exactly that; the
        # legacy descriptor-set UBO path instead leaves the uniforms in an
        # indirect resource table that Piglet never feeds from glUniform*.
        layout = ("layout(push_constant) uniform KodiUBO {"
                  if push_constants else
                  "layout(set = 0, binding = 0, std140) uniform KodiUBO {")
        header.append(layout)
        for _name, decl in block_members:
            header.append("  " + decl + ";")
        header.append("};")
    return ("\n".join(header) + "\n" + body).encode()


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed:\n{r.stdout}\n{r.stderr}")
    return r


# ---------------------------------------------------------------------------
# PSSL parameter table synthesis (Piglet reflection).
#
# psbc emits an all-zero PsslBinaryParamInfo after the shader code.  Piglet
# builds glGetUniformLocation / glGetAttribLocation from the tables that
# follow it (recovered from ShaccVSH-produced blobs):
#
#   PsslBinaryParamInfo (0x1c)
#   PsslBinaryBuffer[numbufferresources]   (0x18 each; samplers + __GLOBAL_CB__)
#   PsslBinaryConstant[numconstants]       (0x20 each; one per vec4 register)
#   PsslBinaryElement[numvariables]        (0x24 each; one per uniform)
#   PsslBinarySamplerState[numsamplerresources] (0x10 each)
#   PsslBinaryAttribute[numinputattributes] (0x10 each)
#   string table (stringtablesize bytes)
#
# nameoff fields are relative to the field itself.

PSSL_TYPE_FLOAT = {1: 0, 2: 1, 3: 2, 4: 3}  # vecN -> PsslType
PSSL_MATRIX = {(2, 2): 35, (2, 3): 30, (2, 4): 35}
PSSL_INT1 = 8
PSSL_BUF_CBUFFER = 22
PSSL_BUF_SRV = 2
PSSL_INTERNAL_CBUFFER = 4
PSSL_INTERNAL_SAMPLER = 5
PSSL_ATTR_CUSTOM = 36


def spv_member_offsets(spv: bytes):
    """Return [(member_index, offset)] for OpMemberDecorate Offset (35)."""
    words = struct.unpack("<%dI" % (len(spv) // 4), spv[: len(spv) // 4 * 4])
    if words[0] != 0x07230203:
        raise RuntimeError("not a SPIR-V module")
    i = 5
    members = []
    while i < len(words):
        wc = words[i] >> 16
        op = words[i] & 0xFFFF
        if wc == 0:
            break
        if op == 72 and wc >= 5:  # OpMemberDecorate
            _struct, member, decoration = words[i + 1], words[i + 2], words[i + 3]
            if decoration == 35 and i + 4 < len(words):  # Offset
                members.append((member, words[i + 4]))
        i += wc
    return members


def es_uniforms(source: bytes):
    """Parse the ES 1.00 source: returns (opaque_samplers, plain_uniforms, attributes, varyings)."""
    import re

    text = source.decode()
    samplers, uniforms, attrs, varyings = [], [], [], []
    for m in re.finditer(
        r"^\s*uniform\s+(?:(?:low|medi|high)p\s+)?(\w+)\s+(\w+)\s*;\s*(?://.*)?$", text, re.M
    ):
        ty, name = m.group(1), m.group(2)
        (samplers if ty.startswith("sampler") else uniforms).append((name, ty))
    for m in re.finditer(
        r"^\s*attribute\s+(?:(?:low|medi|high)p\s+)?(\w+)\s+(\w+)\s*;\s*(?://.*)?$", text, re.M
    ):
        attrs.append((m.group(2), m.group(1)))
    for m in re.finditer(
        r"^\s*varying\s+(?:(?:low|medi|high)p\s+)?(\w+)\s+(\w+)\s*;\s*(?://.*)?$", text, re.M
    ):
        varyings.append((m.group(2), m.group(1)))
    return samplers, uniforms, attrs, varyings


def pssl_type(glsl_type: str) -> int:
    return {
        "float": 0,
        "vec2": 1,
        "vec3": 2,
        "vec4": 3,
        "int": 8,
        "ivec2": 9,
        "ivec3": 10,
        "ivec4": 11,
        "uint": 12,
        "bool": 12,  # Shacc emits gl_FrontFacing as UINT1
        "mat2": 25,
        "mat3": 30,
        "mat4": 35,
    }.get(glsl_type, 3)


def uniform_valued_type(glsl_type: str) -> int:
    """PsslType used in the per-uniform element entry."""
    return pssl_type(glsl_type)


def uniform_registers(glsl_type: str):
    """Yield (element_suffix, etype, byte_delta) per vec4 register slot."""
    if glsl_type == "mat4":
        return [(f"[{i}]", 3, i * 16) for i in range(4)]
    if glsl_type == "mat3":
        return [(f"[{i}]", 2, i * 16) for i in range(3)]
    if glsl_type == "mat2":
        return [(f"[{i}]", 1, i * 16) for i in range(2)]
    return [("", pssl_type(glsl_type), 0)]


def uniform_size(glsl_type: str) -> int:
    return {"float": 4, "vec2": 8, "vec3": 12, "vec4": 16, "int": 4, "uint": 4,
            "bool": 4, "mat2": 32, "mat3": 48, "mat4": 64}.get(glsl_type, 16)


def gnm_active_io(psb: bytes):
    """Return the active GNM input/output locations from the compiled stage.

    psbc keeps the actual varying/attribute locations in the GNM semantic
    tables (the semantic value for user varyings is 15 + location).  The
    PSSL attribute table must only describe these active interfaces:
    declared-but-unused varyings are optimized out of the SPIR-V and have
    no GNM semantic, and listing them makes Piglet's linker fail.
    """
    shf = 0x24
    if psb[shf:shf + 4] != b"Shdr":
        return None
    typ = psb[shf + 8]
    st = 0x34
    common = struct.unpack_from("<I", psb, st)[0]
    nslots = (common >> 24) & 0xFF
    if typ == 1:  # VS
        nis = psb[st + 0x24]
        nes = psb[st + 0x25]
        p = st + 0x28 + 4 * nslots
        attr_locs = {psb[p + 4 * i] for i in range(nis)}
        p += 4 * nis
        out_locs = {struct.unpack_from("<H", psb, p + 2 * i)[0] & 0xFF
                    for i in range(nes)}
        return attr_locs, {v - 15 for v in out_locs}, set()
    if typ == 2:  # PS
        nis = psb[st + 0x38]
        p = st + 0x3c + 4 * nslots
        in_locs = {struct.unpack_from("<H", psb, p + 2 * i)[0] & 0xFF
                   for i in range(nis)}
        return set(), set(), {v - 15 for v in in_locs}
    return None


def build_param_tables(source: bytes, stage: str, spv: bytes, psb: bytes,
                       include_attrs=True, include_uniforms=True,
                       include_samplers=True, varying_locations=None,
                       legacy_attr_sem=False) -> bytes:
    """Replace psbc's zeroed PsslBinaryParamInfo with a real parameter table."""
    if struct.unpack_from("<I", psb, 0)[0] >= 0x10000000:
        raise RuntimeError("unexpected pssl header")
    codesize = struct.unpack_from("<I", psb, 16)[0]
    code_end = 36 + codesize
    if psb[code_end:] != b"\0" * 28:
        raise RuntimeError(
            f"unexpected trailing bytes after shader code ({len(psb) - code_end}), "
            "psbc layout changed?"
        )
    shader = bytearray(psb[:code_end])
    # ShaccVSH marks its output as compiler revision 0xBEEF / type PSSL(2)
    shader[2:4] = struct.pack("<H", 0xBEEF)
    shader[15] = 0x02
    shader = bytes(shader)

    if not include_attrs and not include_uniforms and not include_samplers:
        return psb  # keep psbc's zeroed paraminfo (control variant)

    samplers, uniforms, attrs, varyings = es_uniforms(source)
    if not include_uniforms:
        uniforms = []
    if not include_samplers:
        samplers = []
    offsets = dict(spv_member_offsets(spv))
    if uniforms and len(offsets) != len(uniforms):
        raise RuntimeError(
            f"{stage}: {len(uniforms)} uniforms but {len(offsets)} SPIR-V member offsets"
        )

    constants = []  # (name, elemoff, regidx, etype)
    elements = []   # (name, type, byteoff, size)
    cb_end = 0
    for i, (name, ty) in enumerate(uniforms):
        off = offsets.get(i, 0)
        sz = uniform_size(ty)
        cb_end = max(cb_end, off + sz)
        for suffix, etype, delta in uniform_registers(ty):
            elemoff = off + delta
            constants.append((name + suffix, elemoff, elemoff // 16, etype))
        elements.append((name, uniform_valued_type(ty), off, sz))

    # table counts
    sampler_buffers = [(name, i) for i, (name, _ty) in enumerate(samplers)]
    num_buffers = len(sampler_buffers) + (1 if uniforms else 0)
    num_constants = len(constants)
    num_variables = len(elements)
    num_samplers = 0
    if not include_attrs:
        attrs, varyings = [], []
    # inputs + outputs (Piglet links VS outputs with FS inputs by this table)
    vloc = varying_locations or {}
    active = gnm_active_io(psb) if include_attrs else None
    active_attrs, active_outputs, active_inputs = active or (None, None, None)
    if stage == "vertex":
        input_defs = [(n, t, i, "user") for i, (n, t) in enumerate(attrs)
                      if active_attrs is None or i in active_attrs]
        output_defs = [(n, t, i, "user") for i, (n, t) in enumerate(varyings)
                       if active_outputs is None or i in active_outputs]
        output_defs.append(("gl_Position", "vec4", len(varyings) + 1, "builtin"))
    else:
        input_defs = [(n, t, vloc.get(n, i), "user")
                      for i, (n, t) in enumerate(varyings)
                      if active_inputs is None or vloc.get(n, i) in active_inputs]
        text = source.decode()
        # Built-ins live after the user varyings; Shacc reserves one slot
        # after the highest user location (N) and starts at N+1.
        builtin_base = max((ri for _n, _t, ri, _k in input_defs), default=-1) + 2
        if "gl_FragCoord" in text:
            input_defs.append(("gl_FragCoord", "vec4", builtin_base, "builtin"))
        if "gl_FrontFacing" in text:
            idx = builtin_base + (1 if "gl_FragCoord" in text else 0)
            input_defs.append(("gl_FrontFacing", "bool", idx, "builtin"))
        output_defs = [("gl_FragColor", "vec4", 0, "builtin")]
    attr_defs = input_defs + output_defs
    num_input_attrs, num_output_attrs = len(input_defs), len(output_defs)

    strings = bytearray()

    def add_string(s: str) -> int:
        b = s.encode() + b"\0"
        off = len(strings)
        strings.extend(b)
        return off

    const_name_offsets = []
    for name, _elemoff, _regidx, _etype in constants:
        const_name_offsets.append(add_string(name))
    name_offsets = {}
    for name, _ty in uniforms:
        name_offsets[name] = add_string(name)
    for name, _ in sampler_buffers:
        name_offsets[name] = add_string(name)
    for _name, _ty, _ri, _kind in input_defs:
        name_offsets.setdefault(_name, add_string(_name))
    for _name, _ty, _ri, _kind in output_defs:
        name_offsets.setdefault(_name, add_string(_name))
    global_cb_name = add_string("__GLOBAL_CB__")
    no_name = add_string("(no_name)")

    # PSSL semantic names follow ShaccVSH: ATTR_* for vertex inputs,
    # VAR_* for varyings (VS outputs and PS inputs).  Kodi looks attributes
    # up by their GLSL name, so the standard names below only affect the
    # linker/fetch setup.
    ATTR_SEMANTICS = {
        "m_attrpos": "POSITION", "a_pos": "POSITION",
        "m_attrcol": "COLOR", "a_color": "COLOR",
        "m_attrcord0": "TEXCOORD0", "a_tex": "TEXCOORD0",
        "m_attrcord1": "TEXCOORD1",
        "m_attrsnap": "TEXCOORD2",
    }

    def semantic_name(name: str, kind: str) -> str:
        if kind == "builtin":
            return "(no_name)"
        if legacy_attr_sem:
            return "ATTR_" + name.upper()
        if stage == "vertex" and name in {n for n, _t in attrs}:
            return "ATTR_" + ATTR_SEMANTICS.get(name, name.upper().replace("_", ""))
        return "VAR_" + name.upper().replace("_", "")

    attr_sem_names = {}
    for name, _ty, _ri, kind in input_defs + output_defs:
        attr_sem_names[name] = ((no_name if kind == "builtin"
                                 else add_string(semantic_name(name, kind))))

    # --- layout of the table block -------------------------------------
    # We need the absolute position of every nameoff field to make the
    # self-relative offsets. Build the block with placeholder offsets first.
    paraminfo_len = 28
    buffers_len = 24 * num_buffers
    constants_len = 32 * num_constants
    elements_len = 36 * num_variables
    samplers_len = 16 * num_samplers
    attrs_len = 16 * len(attr_defs)
    table_start = 0  # relative to the start of the param area
    string_start = paraminfo_len + buffers_len + constants_len + elements_len + samplers_len + attrs_len

    def rel(field_abs: int, string_off: int) -> int:
        return string_start + string_off - field_abs

    pi = bytearray()
    pi += struct.pack("<IIII", num_buffers, num_constants, num_variables, num_samplers)
    pi += struct.pack("<BBBB", num_input_attrs, num_output_attrs, 0, 0)
    pi += struct.pack("<II", 0, len(strings))

    bufs = bytearray()
    field_abs = paraminfo_len
    for name, ridx in sampler_buffers:
        bufs += struct.pack("<IIBBBBII", ridx, 0, PSSL_BUF_SRV, PSSL_INTERNAL_SAMPLER,
                            pssl_type("vec4"), 0, 0, 0)
        bufs += struct.pack("<I", rel(field_abs + 20, name_offsets[name]))
        field_abs += 24
    if uniforms:
        bufs += struct.pack("<IIBBBBII", 0, cb_end, PSSL_BUF_CBUFFER,
                            PSSL_INTERNAL_CBUFFER, 106, 0, len(uniforms), 0)
        bufs += struct.pack("<I", rel(field_abs + 20, global_cb_name))
        field_abs += 24

    consts = bytearray()
    for (name, elemoff, regidx, etype), name_off in zip(constants, const_name_offsets):
        consts += struct.pack("<IIBBBB", elemoff, regidx, 0, etype, PSSL_BUF_CBUFFER, 0)
        consts += b"\0" * 16  # default value
        consts += struct.pack("<I", rel(field_abs + 28, name_off))
        field_abs += 32

    elems = bytearray()
    for name, ty, byteoff, size in elements:
        elems += struct.pack("<BBBBI", ty, 1, 0, 3, byteoff)
        elems += struct.pack("<III", size, 0, 0xFFFFFFFF)
        elems += struct.pack("<II", 0, 0)
        elems += struct.pack("<I", rel(field_abs + 28, name_offsets[name]))
        elems += struct.pack("<I", rel(field_abs + 32, no_name))
        field_abs += 36

    BUILTIN_SEMANTICS = {"gl_Position": 46, "gl_FragCoord": 46, "gl_FragColor": 56,
                         "gl_FrontFacing": 53}
    attrsblk = bytearray()
    for name, ty, ri, kind in attr_defs:
        sem = BUILTIN_SEMANTICS.get(name, PSSL_ATTR_CUSTOM)
        attrsblk += struct.pack("<BBBBBBH", pssl_type(ty), sem, 0, ri, 0, 0, 0)
        attrsblk += struct.pack("<I", rel(field_abs + 8, name_offsets[name]))
        attrsblk += struct.pack("<I", rel(field_abs + 12, attr_sem_names[name]))
        field_abs += 16

    param = pi + bufs + consts + elems + attrsblk + bytes(strings)
    return shader + param


# Temporary A/B map (handler35): which parametric sections go into each
# source, to isolate what makes Piglet's glLinkProgram fail.
VARIANTS_BY_SOURCE = {}


def variant_for(fname: str):
    return VARIANTS_BY_SOURCE.get(
        fname, {"include_attrs": True, "include_uniforms": True, "include_samplers": True}
    )


def create_blob(final_source: bytes, sb: bytes) -> bytes:
    return (
        struct.pack("<IIII", FORMAT, MAGIC, VERSION, 1)
        + struct.pack("<I", len(final_source))
        + final_source
        + struct.pack("<I", len(sb))
        + sb
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shaders-dir", required=True, help="GLES/2.0 shader source dir")
    ap.add_argument("--psbc", default="/tmp/kodi-ps4/psbc/psbc")
    ap.add_argument("--out", required=True, help="output dir for <hash>.bin blobs")
    ap.add_argument("--legacy-ubo", action="store_true",
                    help="keep the old descriptor-set UBO path (A/B control)")
    ap.add_argument("--legacy-attr-sem", action="store_true",
                    help="use the old ATTR_* semantic names for varyings (A/B)")
    ap.add_argument("--no-attrs", action="store_true",
                    help="omit the PSSL attribute table (link control)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    total = 0
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for vert, frag in GUI_SHADERS:
            for di, defines in enumerate(DEFINES_VARIANTS):
                for stage, fname in (("vertex", vert), ("fragment", frag)):
                    path = os.path.join(args.shaders_dir, fname)
                    final = insert_prefix(open(path, "rb").read(), defines)
                    # IO locations are assigned per (vert, frag) pair so VS
                    # outputs and FS inputs land on the same interfaces.  The
                    # FS must use the VS varying order even when it declares
                    # only a subset, otherwise its locations shift.
                    vfs = open(os.path.join(args.shaders_dir, vert), "rb").read()
                    attrs, vs_varyings = extract_io(vfs)
                    if stage == "vertex":
                        varyings = vs_varyings
                    else:
                        _, varyings = extract_io(open(os.path.join(args.shaders_dir, frag), "rb").read())
                        missing = [n for n in varyings if n not in vs_varyings]
                        if missing:
                            raise RuntimeError(
                                f"{frag}: varyings not declared by {vert}: {missing}"
                            )
                    attr_location = {n: i for i, n in enumerate(attrs)} if stage == "vertex" else None
                    varying_location = {n: i for i, n in enumerate(vs_varyings)}
                    # The blob embeds and is keyed by the exact ES 1.00 source
                    # that Kodi assembles and hashes at runtime; SPIR-V is
                    # produced from the ES 3.10 translation of the same text.
                    spvsrc = translate_es100_to_es310(
                        stage, final, attr_location, varying_location,
                        push_constants=not args.legacy_ubo
                    )
                    srcpath = os.path.join(tmp, "cur.glsl")
                    with open(srcpath, "wb") as fh:
                        fh.write(spvsrc)
                    hash_ = djb2(final)
                    try:
                        spv = os.path.join(tmp, "cur.spv")
                        run(["glslangValidator", "-V", "-R", "-S", "vert" if stage == "vertex" else "frag","-o", spv, srcpath])
                        sb = os.path.join(tmp, "cur.sb")
                        run([args.psbc, "-s", stage, "-f", spv, "-o", sb])
                        var = variant_for(fname)
                        psb = build_param_tables(
                            final, stage, open(spv, "rb").read(), open(sb, "rb").read(),
                            include_attrs=var["include_attrs"] and not args.no_attrs,
                            include_uniforms=var["include_uniforms"],
                            include_samplers=var.get("include_samplers", True),
                            varying_locations=varying_location,
                            legacy_attr_sem=args.legacy_attr_sem,
                        )
                        blob = create_blob(final, psb)
                        with open(os.path.join(args.out, f"{hash_:08x}.bin"), "wb") as fh:
                            fh.write(blob)
                        total += 1
                    except Exception as e:  # noqa
                        failures.append((hash_, fname, str(e)))

    print(f"generated {total} blobs in {args.out}")
    for h, f, msg in failures:
        for i, line in enumerate(msg.splitlines()):
            tag = f"FAILED {h:08x} {f}: " if i == 0 else f"           "
            print(tag + line, file=sys.stderr)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
