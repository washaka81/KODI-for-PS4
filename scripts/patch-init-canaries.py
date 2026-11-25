#!/usr/bin/env python3
"""Spread the start up canary over the init array to locate a failing constructor.

The static constructors of the core are called by the C runtime of the SDK,
which walks the init array from its first entry up to the dynamic table.  The
canary of the port is one of those constructors, so it only tells that the
constructors started, not which one died.

This script replaces the constructor of some init array entries with the
canary, so the canary runs at those points and its counter says how far the
process got.  The canary prints "ctor-<n>", so the last number in the trace
is the position where the process died.

The original constructor of a replaced entry is not executed, but its name is
kept in the report written next to the binary, so the culprit can be looked
up when the trace stops at that point.

Usage:
  ./scripts/patch-init-canaries.py <elf> [--step N] [--report FILE]
"""
import argparse
import struct
import sys

SHN_UNDEF = 0
R_X86_64_RELATIVE = 8
INIT_ARRAY = ".init_array"
CANARY = "CanaryConstructors"


def sections(data):
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sh = [struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize)
          for i in range(e_shnum)]
    shstr = sh[e_shstrndx][4]

    def name_of(section):
        end = data.find(b"\0", shstr + section[0])
        return data[shstr + section[0]:end].decode("utf-8", "replace")

    return {name_of(s): s for s in sh}, sh


def symbols(data, sh):
    sym = sh_by_name(sh, data, ".symtab")
    strtab = sh[sym[6]][4]
    out = []
    for i in range(sym[5] // 24):
        st_name, _info, _other, st_shndx, st_value, _size = struct.unpack_from(
            "<IBBHQQ", data, sym[4] + i * 24)
        if st_value:
            end = data.find(b"\0", strtab + st_name)
            out.append((st_value, data[strtab + st_name:end].decode("utf-8", "replace")))
    out.sort()
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf")
    parser.add_argument("--step", type=int, default=50)
    parser.add_argument("--positions", type=lambda s: [int(p) for p in s.split(",")],
                        default=None)
    parser.add_argument("--report")
    args = parser.parse_args()

    data = bytearray(open(args.elf, "rb").read())

    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
    segs = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, o)[0]
        p_offset, p_vaddr, _p_paddr, p_filesz, _p_memsz, _p_align = struct.unpack_from(
            "<QQQQQQ", data, o + 8)
        segs.append((p_type, p_offset, p_vaddr, p_filesz))

    def vaddr_to_offset(vaddr):
        for p_type, p_offset, p_vaddr, p_filesz in segs:
            if p_type == 1 and p_vaddr <= vaddr < p_vaddr + p_filesz:
                return p_offset + (vaddr - p_vaddr)
        return None

    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sh = [struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize)
          for i in range(e_shnum)]
    shstr = sh[e_shstrndx][4]

    def name_of(section):
        end = data.find(b"\0", shstr + section[0])
        return data[shstr + section[0]:end].decode("utf-8", "replace")

    by_name = {name_of(s): s for s in sh}

    init = by_name[INIT_ARRAY]
    count = init[5] // 8

    # The address of the canary.
    symtab = by_name[".symtab"]
    strtab = sh[symtab[6]][4]
    canary = None
    names = []
    for i in range(symtab[5] // 24):
        st_name, _info, _other, _shndx, st_value, _size = struct.unpack_from(
            "<IBBHQQ", data, symtab[4] + i * 24)
        end = data.find(b"\0", strtab + st_name)
        name = data[strtab + st_name:end].decode("utf-8", "replace")
        if name and st_value:
            names.append((st_value, name))
        if CANARY in name:
            canary = st_value
    if canary is None:
        print("ERROR: no se encuentra el canario en el binario", file=sys.stderr)
        return 1
    names.sort()

    # The relocations that fill the init array.
    rela = by_name[".rela.dyn"]
    rela_off = vaddr_to_offset(rela[3])
    addends = {}
    for i in range(rela[5] // 24):
        r_offset, r_info, r_addend = struct.unpack_from("<QQq", data, rela_off + i * 24)
        if r_info & 0xFFFFFFFF == R_X86_64_RELATIVE and init[3] <= r_offset < init[3] + init[5]:
            addends[r_offset] = (i, r_addend)

    if args.positions:
        positions = args.positions
    else:
        positions = list(range(0, count, args.step))
    print(f"init array: {count} entradas, canario en 0x{canary:x}")
    print(f"canarios en las posiciones: {positions}")

    changed = 0
    report = []
    for index, pos in enumerate(positions):
        offset = init[3] + pos * 8
        if offset not in addends:
            print(f"ERROR: la entrada {pos} (0x{offset:x}) no tiene relocalizacion", file=sys.stderr)
            return 1
        index_rel, original = addends[offset]
        struct.pack_into("<q", data, rela_off + index_rel * 24 + 16, canary)
        report.append((pos, original, names))
        changed += 1
    print(f"relocalizaciones cambiadas: {changed}")

    with open(args.elf, "wb") as fh:
        fh.write(bytes(data))

    if args.report:
        import bisect
        values = [v for v, _ in names]
        with open(args.report, "w") as fh:
            for pos, original, _ in report:
                i = bisect.bisect_right(values, original) - 1
                name = names[i][1] if i >= 0 else "?"
                fh.write(f"{pos}\t0x{original:x}\t{name}\n")
        print(f"informe escrito en {args.report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
