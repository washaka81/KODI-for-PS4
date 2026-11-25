#!/usr/bin/env python3
"""Resolve a symbol name to an address in a PS4 SPRX using its NID table.

Usage:
  ./scripts/ps4-sprx-nid.py <sprx> <symbol> [symbol...]

The SPRX may be stripped of section headers (PS4 modules often are).  The
script reads the program headers, locates PT_SCE_DYNLIBDATA and the dynamic
table, and parses the NID/symbol tables described in the OpenOrbis ELF spec.
"""

import base64
import hashlib
import struct
import sys

PT_SCE_DYNLIBDATA = 0x61000000
PT_LOAD = 1
PT_DYNAMIC = 2

TAG_STRTAB = 0x61000035
TAG_STRSZ = 0x61000037
TAG_SYMTAB = 0x61000039
TAG_SYMENT = 0x6100003B
TAG_SYMTABSZ = 0x6100003F

NID_SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")


def nid(name: str) -> str:
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    return base64.b64encode(digest[:8][::-1]).decode()[:-1].replace("/", "-")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2

    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"\x7fELF":
        print("not an ELF", file=sys.stderr)
        return 2

    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]

    dynlib = None
    dynamic = None
    loads = []
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = (
            struct.unpack_from("<IIQQQQQQ", data, off)
        )
        if p_type == PT_SCE_DYNLIBDATA:
            dynlib = (p_offset, p_filesz)
        elif p_type == PT_DYNAMIC:
            dynamic = (p_offset, p_filesz)
        elif p_type == PT_LOAD:
            loads.append((p_offset, p_vaddr, p_filesz))

    if dynlib is None or dynamic is None:
        print("missing PT_SCE_DYNLIBDATA/PT_DYNAMIC", file=sys.stderr)
        return 2

    dyn_off, dyn_size = dynamic
    tags = {}
    for i in range(dyn_size // 16):
        tag, val = struct.unpack_from("<QQ", data, dyn_off + i * 16)
        if tag == 0:
            break
        tags[tag] = val

    for tag in (TAG_STRTAB, TAG_STRSZ, TAG_SYMTAB, TAG_SYMENT, TAG_SYMTABSZ):
        if tag not in tags:
            print(f"missing dynamic tag {tag:#x}", file=sys.stderr)
            return 2

    base = dynlib[0]
    strtab = base + tags[TAG_STRTAB]
    strsz = tags[TAG_STRSZ]
    symtab = base + tags[TAG_SYMTAB]
    syment = tags[TAG_SYMENT]
    symsz = tags[TAG_SYMTABSZ]

    # Build NID -> (address,size) from the symbol table.  The first 0x30
    # bytes are reserved; entries are Elf64_Sym (24 bytes).
    resolved = {}
    for i in range(symsz // syment):
        off = symtab + i * syment
        if off + syment > len(data):
            break
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", data, off
        )
        if st_name >= strsz:
            continue
        end = data.find(b"\0", strtab + st_name)
        if end < 0:
            continue
        name = data[strtab + st_name : end].decode("latin1")
        if not name or "#" not in name:
            continue
        resolved[name[:11]] = (st_value, st_size)

    for symbol in sys.argv[2:]:
        key = nid(symbol)
        hit = resolved.get(key)
        if hit:
            print(f"{symbol} NID={key} value=0x{hit[0]:x} size=0x{hit[1]:x}")
        else:
            print(f"{symbol} NID={key} NOT FOUND")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
