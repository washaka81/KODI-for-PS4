#!/usr/bin/env python3
"""Make the relocations of a PIE compatible with the PS4 loader.

The loader of the PS4 only applies R_X86_64_RELATIVE and R_X86_64_JUMP_SLOT.
An executable that has other relocation types is rejected (or does not get its
imports resolved) and does not start.

The offending relocations come from the prebuilt archives of the SDK, whose
symbols are exported and can not be bound locally:

* R_X86_64_64: the address of an imported symbol stored in a table, for
  example the VFS of SQLite, which takes the address of the libc functions.
* R_X86_64_GLOB_DAT: the address of an imported symbol stored in the GOT.

Both store the address of an imported symbol, which is what
R_X86_64_JUMP_SLOT does as well, so the type is changed and the symbol
resolution is left to the loader.
"""
import struct
import sys

R_X86_64_RELATIVE = 8
R_X86_64_JUMP_SLOT = 7
R_X86_64_64 = 1
R_X86_64_GLOB_DAT = 6
SHT_RELA = 4
SHT_DYNSYM = 11
SHN_UNDEF = 0


def patch(path):
    data = bytearray(open(path, "rb").read())
    if data[:4] != b"\x7fELF":
        sys.exit(f"{path} is not an ELF file")

    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", data, off)
        link, info = struct.unpack_from("<II", data, off + 40)
        sections.append((name, typ, flags, addr, offset, size, link, info))

    dynsym = None
    for name, typ, flags, addr, offset, size, link, info in sections:
        if typ == SHT_DYNSYM:
            dynsym = (offset, size, link)
    if dynsym is None:
        sys.exit(f"{path} has no dynamic symbol table")

    shstr_offset = sections[e_shstrndx][4]

    def section_name(name_offset):
        end = data.find(b"\0", shstr_offset + name_offset)
        return data[shstr_offset + name_offset:end].decode("utf-8", "replace")

    by_name = {section_name(section[0]): section for section in sections}
    dynstr_offset = sections[dynsym[2]][4]

    def symbol_name(name_offset):
        end = data.find(b"\0", dynstr_offset + name_offset)
        return data[dynstr_offset + name_offset:end].decode("utf-8", "replace")

    # The PS4 loader only resolves JUMP_SLOT entries.  Addresses of imported
    # functions stored in tables are R_X86_64_64/GLOB_DAT; point them at the
    # local PLT stub, which already has the JUMP_SLOT entry the loader fills.
    plt_by_sym = {}
    plt = by_name.get(".plt")
    rela_plt = by_name.get(".rela.plt")
    if plt and rela_plt:
        plt_addr = plt[3]
        for i in range(rela_plt[5] // 24):
            pos = rela_plt[4] + i * 24
            _, r_info, r_sym, _ = struct.unpack_from("<QLLq", data, pos)
            if (r_info & 0xFFFFFFFF) == R_X86_64_JUMP_SLOT:
                plt_by_sym[r_sym] = plt_addr + 16 + i * 16

    converted = 0
    without_stub = []
    for name, typ, flags, addr, offset, size, link, info in sections:
        if typ != SHT_RELA:
            continue
        for i in range(size // 24):
            pos = offset + i * 24
            r_offset, r_info, r_sym, r_addend = struct.unpack_from("<QLLq", data, pos)
            rtype = r_info & 0xFFFFFFFF
            if rtype not in (R_X86_64_64, R_X86_64_GLOB_DAT):
                continue
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                "<IBBHQQ", data, dynsym[0] + r_sym * 24)
            if st_shndx != SHN_UNDEF:
                # The symbol is defined by the executable, a relative relocation
                # is enough.
                struct.pack_into("<QLLq", data, pos, r_offset, R_X86_64_RELATIVE, 0, st_value + r_addend)
            else:
                stub = plt_by_sym.get(r_sym)
                if stub is None:
                    without_stub.append(symbol_name(st_name))
                    stub = 0
                struct.pack_into("<QLLq", data, pos, r_offset, R_X86_64_RELATIVE, 0, stub)
            converted += 1

    with open(path, "wb") as f:
        f.write(data)
    print(f"{path}: {converted} relocations changed to a type the PS4 loader applies")
    if without_stub:
        print(f"{path}: imported addresses without PLT stub: {', '.join(sorted(set(without_stub)))}")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        patch(arg)
