#!/usr/bin/env python3
"""Neutraliza el dispatch ADX/BMI2 de OpenSSL P-256 en el ELF de Kodi.

OpenSSL elige __ecp_nistz256_*_montx cuando OPENSSL_ia32cap_P[2] tiene
BMI2 (bit 8) y ADX (bit 19).  El Jaguar de PS4 no ejecuta mulx/adcx/adox.
El dispatch compila a:

    b9 00 01 08 00   mov $0x80100,%ecx
    23 0d ...        and OPENSSL_ia32cap_P+8,%ecx
    81 f9 00 01 08 00 cmp $0x80100,%ecx

Cambiar el inmediato del mov por 0 hace que el and dé 0, el cmp no
coincida y se use el path q (mul/adc), sin tocar la lista de curvas.
El parche es idempotente y falla si no encuentra el patrón esperado.
"""

from __future__ import annotations

import sys
from pathlib import Path

MOV = bytes.fromhex("b9 00 01 08 00")
AND = bytes.fromhex("23 0d")
CMP = bytes.fromhex("81 f9 00 01 08 00")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"uso: {sys.argv[0]} <kodi-core.elf>", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    data = bytearray(path.read_bytes())
    sites: list[int] = []
    changed = 0
    pos = 0
    while True:
        pos = data.find(MOV, pos)
        if pos < 0:
            break
        window = data[pos : pos + 64]
        if AND in window[:8] and CMP in window:
            sites.append(pos)
            if data[pos + 1 : pos + 5] != b"\x00\x00\x00\x00":
                data[pos + 1 : pos + 5] = b"\x00\x00\x00\x00"
                changed += 1
        pos += len(MOV)
    if not sites:
        print("error: no se encontró el dispatch ecp_nistz256 ADX", file=sys.stderr)
        return 1
    path.write_bytes(data)
    print(
        "OpenSSL ADX neutralizado: "
        f"{len(sites)} sitios, {changed} modificados: "
        + ", ".join(f"0x{site:x}" for site in sites)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
