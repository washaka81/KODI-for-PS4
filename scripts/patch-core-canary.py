#!/usr/bin/env python3
"""Cirugía ELF sobre el core de Kodi para instrumentar el arranque.

El `link.x` del SDK solo recoge `*(.init_array)`, de modo que las secciones
`.init_array.NNN` (prioridad de constructor) se quedan fuera del array y sus
constructores nunca se ejecutan.  Este script reinyecta el canario en la
primera posicion del `.init_array` real, de forma que se pueda saber si la
aplicacion llega a los constructores o muere antes.

Opciones:
  --put-canary   copia el puntero de `.init_array.101` a la primera entrada
                 de `.init_array` (el canario se ejecuta el primero).
  --canary-last  pone el canario en la ULTIMA entrada del `.init_array`: si
                 aparece, todos los constructores han corrido.
  --no-init      pone DT_INIT_ARRAYSZ a 0 (no se ejecuta ningun constructor).
  --one-ctor     deja una sola entrada en el .init_array (la del canario).
  --init-size N  deja solo las N primeras entradas del .init_array (bisecado).
  --no-tls       deja el segmento PT_TLS sin tamano (prueba del TLS).
  --entry-canary  redirige el punto de entrada al canario: si el log aparece,
                 el loader llega a ejecutar codigo y el fallo es posterior.
  --report       solo informa del estado, no modifica nada.
"""

import argparse
import struct
import sys

SHT_PROGBITS = 1
SHT_INIT_ARRAY = 14
SHT_NOBITS = 8
DT_INIT_ARRAY = 0x19
DT_INIT_ARRAYSZ = 0x1B
PT_DYNAMIC = 2
PT_LOAD = 1


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.data = bytearray(fh.read())
        d = self.data
        if d[:4] != b"\x7fELF":
            raise SystemExit(f"{path}: no es un ELF")
        self.e_phoff, = struct.unpack_from("<Q", d, 0x20)
        self.e_shoff, = struct.unpack_from("<Q", d, 0x28)
        self.e_phentsize, self.e_phnum = struct.unpack_from("<HH", d, 0x36)
        self.e_shentsize, self.e_shnum, self.e_shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            self.sections.append(struct.unpack_from("<IIQQQQIIQQ", d, off))
        stroff = self.sections[self.e_shstrndx][4]
        self.names = []
        for s in self.sections:
            end = d.find(b"\0", stroff + s[0])
            self.names.append(d[stroff + s[0]:end].decode(errors="replace"))

    def find_section(self, name):
        for i, n in enumerate(self.names):
            if n == name:
                return i
        return None

    def dyn_entries(self):
        """Devuelve (offset_del_tag_en_PT_DYNAMIC, lista_de_(tag,val,pos))."""
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            p_type, = struct.unpack_from("<I", self.data, off)
            if p_type != PT_DYNAMIC:
                continue
            p_offset, = struct.unpack_from("<Q", self.data, off + 8)
            p_filesz, = struct.unpack_from("<Q", self.data, off + 32)
            entries = []
            for j in range(p_filesz // 16):
                pos = p_offset + j * 16
                tag, val = struct.unpack_from("<qQ", self.data, pos)
                entries.append((tag, val, pos))
                if tag == 0:
                    break
            return entries
        raise SystemExit("sin PT_DYNAMIC")

    def vaddr_to_offset(self, vaddr):
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            p_type, = struct.unpack_from("<I", self.data, off)
            if p_type != PT_LOAD:
                continue
            p_offset, p_vaddr = struct.unpack_from("<QQ", self.data, off + 8)
            p_filesz, = struct.unpack_from("<Q", self.data, off + 32)
            if p_vaddr <= vaddr < p_vaddr + p_filesz:
                return p_offset + (vaddr - p_vaddr)
        return None

    def save(self):
        with open(self.path, "wb") as fh:
            fh.write(self.data)

    def symbol_value(self, needle):
        """Direccion de un simbolo del .symtab cuyo nombre contenga needle."""
        sec = self.find_section(".symtab")
        if sec is None:
            return None
        s = self.sections[sec]
        # la tabla de nombres de los simbolos es la que indica sh_link, no la
        # de los nombres de seccion
        stroff = self.sections[s[6]][4]
        best = None
        for j in range(s[5] // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                "<IBBHQQ", self.data, s[4] + j * 24)
            end = self.data.find(b"\0", stroff + st_name)
            name = self.data[stroff + st_name:end].decode(errors="replace")
            if needle in name and st_value:
                if best is None or st_size > best[1]:
                    best = (st_value, st_size, name)
        return best

    def rela_for(self, vaddr):
        """Posicion (en .rela.dyn) de la relocalizacion que cubre vaddr."""
        rel = self.find_section(".rela.dyn")
        if rel is None:
            return None
        s = self.sections[rel]
        for j in range(s[5] // 24):
            r_off, = struct.unpack_from("<Q", self.data, s[4] + j * 24)
            if r_off == vaddr:
                return s[4] + j * 24
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--put-canary", action="store_true")
    ap.add_argument("--canary-last", action="store_true")
    ap.add_argument("--no-init", action="store_true")
    ap.add_argument("--one-ctor", action="store_true")
    ap.add_argument("--init-size", type=int, default=0)
    ap.add_argument("--no-tls", action="store_true")
    ap.add_argument("--entry-canary", action="store_true")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    elf = Elf(args.elf)

    ia = elf.find_section(".init_array")
    ip = elf.find_section(".init_array.101")
    if ia is None:
        raise SystemExit("no hay .init_array")
    s = elf.sections[ia]
    print(f".init_array       addr=0x{s[3]:x} off=0x{s[4]:x} size={s[5]} ({s[5] // 8} entradas)")
    if ip is not None:
        p = elf.sections[ip]
        value, = struct.unpack_from("<Q", elf.data, p[4])
        print(f".init_array.101   addr=0x{p[3]:x} off=0x{p[4]:x} size={p[5]} valor=0x{value:x}")
    else:
        print(".init_array.101   no existe")
        value = None

    dyn = elf.dyn_entries()
    for tag, val, _ in dyn:
        if tag in (DT_INIT_ARRAY, DT_INIT_ARRAYSZ):
            print(f"  DT 0x{tag:x} = 0x{val:x} ({val})")

    # Las entradas del .init_array deben estar cubiertas por R_X86_64_RELATIVE
    rel = elf.find_section(".rela.dyn")
    covered = 0
    if rel is not None:
        rs = elf.sections[rel]
        for j in range(rs[5] // 24):
            r_off, r_info = struct.unpack_from("<QQ", elf.data, rs[4] + j * 24)
            rtype = r_info & 0xFFFFFFFF
            if rtype == 8 and s[3] <= r_off < s[3] + s[5]:
                covered += 1
    print(f"  entradas del .init_array cubiertas por R_X86_64_RELATIVE: {covered}/{s[5] // 8}")

    if args.report:
        return

    if args.put_canary:
        canary = elf.symbol_value("CanaryConstructors")
        if canary is None:
            raise SystemExit("no se encuentra el simbolo CanaryConstructors")
        addr, size, name = canary
        print(f"  canario: {name} en 0x{addr:x} (size {size})")
        # El valor real de cada entrada del .init_array esta en el addend de su
        # relocalizacion RELATIVE, no en el contenido de la seccion.
        pos = elf.rela_for(s[3])
        if pos is None:
            raise SystemExit("la primera entrada del .init_array no tiene relocalizacion")
        struct.pack_into("<q", elf.data, pos + 16, addr)
        print(f"  addend de la primera entrada del .init_array -> 0x{addr:x}")

    if args.init_size:
        for tag, val, pos in dyn:
            if tag == DT_INIT_ARRAYSZ:
                struct.pack_into("<Q", elf.data, pos + 8, args.init_size * 8)
                print(f"  DT_INIT_ARRAYSZ = {args.init_size} entradas")

    if args.one_ctor:
        for tag, val, pos in dyn:
            if tag == DT_INIT_ARRAYSZ:
                struct.pack_into("<Q", elf.data, pos + 8, 8)
                print("  DT_INIT_ARRAYSZ = 8 (un solo constructor)")

    if args.entry_canary:
        canary = elf.symbol_value("CanaryConstructors")
        if canary is None:
            raise SystemExit("no se encuentra el simbolo CanaryConstructors")
        addr = canary[0]
        entry, = struct.unpack_from("<Q", elf.data, 0x18)
        struct.pack_into("<Q", elf.data, 0x18, addr)
        print(f"  punto de entrada: 0x{entry:x} -> 0x{addr:x} (canario)")

    if args.canary_last:
        canary = elf.symbol_value("CanaryConstructors")
        if canary is None:
            raise SystemExit("no se encuentra el simbolo CanaryConstructors")
        addr = canary[0]
        # la ultima entrada del .init_array
        last_addr = s[3] + s[5] - 8
        pos = elf.rela_for(last_addr)
        if pos is None:
            raise SystemExit("la ultima entrada del .init_array no tiene relocalizacion")
        struct.pack_into("<q", elf.data, pos + 16, addr)
        print(f"  canario en la ultima entrada del .init_array (0x{last_addr:x} -> 0x{addr:x})")

    if args.no_tls:
        for i in range(elf.e_phnum):
            off = elf.e_phoff + i * elf.e_phentsize
            p_type, = struct.unpack_from("<I", elf.data, off)
            if p_type == 7:  # PT_TLS
                struct.pack_into("<QQ", elf.data, off + 32, 0, 0)  # p_filesz, p_memsz
                print("  PT_TLS sin tamano (prueba del TLS)")

    if args.no_init:
        for tag, val, pos in dyn:
            if tag == DT_INIT_ARRAYSZ:
                struct.pack_into("<Q", elf.data, pos + 8, 0)
                print("  DT_INIT_ARRAYSZ = 0 (sin constructores)")

    elf.save()
    print(f"  {args.elf} actualizado")


if __name__ == "__main__":
    main()
