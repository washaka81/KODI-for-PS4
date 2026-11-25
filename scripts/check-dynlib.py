#!/usr/bin/env python3
"""Comprueba la coherencia interna del blob SCE_Dynlib_Data de un OELF.

El loader de PS4 resuelve los imports por NID: cada entrada de la tabla de
simbolos apunta (por offset) a una entrada de 16 bytes de la tabla NID, con
el formato  NID#libreria#modulo\\0.  Si la tabla NID es mas corta que la de
simbolos, el loader lee basura y el proceso revienta.

Uso:
  ./scripts/check-dynlib.py build/kodi-core.oelf [otro.oelf ...]
"""

import struct
import sys

DT_SCE_HASH = 0x61000025
DT_SCE_HASHSZ = 0x6100003D
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F
DT_SCE_RELA = 0x6100002F
DT_SCE_RELASZ = 0x61000031
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_PLTGOT = 0x61000027
DT_SCE_DYNLIBDATA = 0x61000025  #复用 con DT_SCE_HASH, se usa el offset del blob


def read_elf(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"\x7fELF":
        raise SystemExit(f"{path}: no es un ELF")
    base = 0x120 if len(data) > 0x140 and data[0x120:0x124] == b"\x7fELF" else 0
    e_shoff, = struct.unpack_from("<Q", data, base + 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, base + 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize)
                for i in range(e_shnum)]
    shstr = sections[e_shstrndx][4]

    def name_of(section):
        end = data.find(b"\0", shstr + section[0])
        return data[shstr + section[0]:end].decode("utf-8", "replace")

    by_name = {name_of(s): s for s in sections}
    # el blob de datos dinamicos esta en el segmento PT_SCE_DYNLIBDATA
    dynlib = None
    for i in range(len(sections)):
        s = sections[i]
        if s[1] == 1:      # SHT_PROGBITS... no: el tipo de seccion no sirve
            pass
    # los segmentos de programa estan en los program headers
    e_phoff, = struct.unpack_from("<Q", data, base + 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, base + 0x36)
    for i in range(e_phnum):
        o = base + e_phoff + i * e_phentsize
        p_type, = struct.unpack_from("<I", data, o)
        if p_type == 0x61000000:      # PT_SCE_DYNLIBDATA
            dynlib = struct.unpack_from("<Q", data, o + 8)[0]
            break

    # la tabla dinamica se lee del segmento PT_DYNAMIC
    tags = {}
    for i in range(e_phnum):
        o = base + e_phoff + i * e_phentsize
        p_type, = struct.unpack_from("<I", data, o)
        if p_type != 2:      # PT_DYNAMIC
            continue
        p_offset, = struct.unpack_from("<Q", data, o + 8)
        p_filesz, = struct.unpack_from("<Q", data, o + 32)
        for j in range(p_filesz // 16):
            tag, val = struct.unpack_from("<qQ", data, p_offset + j * 16)
            if tag == 0:
                break
            tags.setdefault(tag, val)
    return data, base, by_name, tags, sections, dynlib


def segment_offset(sections, base, vaddr):
    """Convierte una direccion virtual del blob en offset de fichero."""
    for s in sections:
        if s[1] != 1:      # PT_LOAD
            continue
        off, va, _pa, fsz, msz, _al = s[2], s[3], s[4], s[5], s[6], s[7]
        if va <= vaddr < va + fsz:
            return base + off + (vaddr - va)
    return None


def check(path):
    print(f"=== {path}")
    data, base, by_name, tags, sections, blob = read_elf(path)
    if blob is None:
        print("  no tiene segmento PT_SCE_DYNLIBDATA")
        return
    print(f"  blob de datos dinamicos en 0x{blob:x}")

    strtab = tags.get(DT_SCE_STRTAB, 0)
    strsz = tags.get(DT_SCE_STRSZ, 0)
    symtab = tags.get(DT_SCE_SYMTAB, 0)
    symsz = tags.get(DT_SCE_SYMTABSZ, 0)
    hashsz = tags.get(DT_SCE_HASHSZ, 0)
    relasz = tags.get(DT_SCE_RELASZ, 0)
    jmprel = tags.get(DT_SCE_JMPREL, 0)
    pltrelsz = tags.get(DT_SCE_PLTRELSZ, 0)

    print(f"  tabla de strings : 0x{strtab:x} ({strsz} bytes)")
    print(f"  tabla de simbolos: 0x{symtab:x} ({symsz} bytes, {symsz // 24} entradas)")
    print(f"  tabla hash       : {hashsz} bytes")
    print(f"  relocalizaciones : {relasz} bytes ({relasz // 24}) + PLT {pltrelsz} bytes")
    print(f"  PLT GOT         : 0x{tags.get(DT_SCE_PLTGOT, 0):x}")

    # La tabla NID esta dentro de la tabla de strings, al final.
    # Los simbolos apuntan con offsets dentro de ella.
    sym_off = segment_offset(sections, base, symtab)
    str_off = segment_offset(sections, base, strtab)
    if sym_off is None or str_off is None:
        print("  no se pueden resolver los offsets")
        return

    entries = []
    for i in range(symsz // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", data, sym_off + i * 24)
        entries.append((st_name, st_info, st_shndx))

    # Los simbolos externos (SHN_UNDEF) son los que tienen NID.
    undef = [e for e in entries if e[2] == 0]
    print(f"  simbolos externos: {len(undef)}")

    # El primer simbolo externo apunta al inicio de la tabla NID.
    if not undef:
        print("  no hay simbolos externos")
        return
    nid_start = undef[0][0]
    print(f"  la tabla NID empieza en el offset {nid_start} de la tabla de strings")

    # Ultimo simbolo externo: necesita 16 bytes mas.
    last = max(e[0] for e in undef)
    needed = last + 16
    print(f"  el ultimo simbolo necesita hasta el offset {needed}"
          f" (la tabla de strings mide {strsz})")
    if needed > strsz:
        print(f"  >>> FALLO: la tabla NID se queda corta en {needed - strsz} bytes")
        return

    # Muestra las primeras entradas NID y las ultimas, para ver si son validas.
    def nid_at(offset):
        pos = str_off + offset
        raw = data[pos:pos + 16]
        return raw.split(b"\0")[0].decode("utf-8", "replace")

    print(f"  primera NID: {nid_at(nid_start)!r}")
    print(f"  ultima  NID: {nid_at(last)!r}")

    # Los simbolos exportados (SHN_UNDEF con st_info global) tambien usan NIDs.
    bad = 0
    for st_name, st_info, st_shndx in entries:
        if st_name >= strsz:
            bad += 1
    if bad:
        print(f"  >>> {bad} simbolos apuntan fuera de la tabla de strings")
    else:
        print("  todos los simbolos apuntan dentro de la tabla de strings")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        check(arg)
        print()
