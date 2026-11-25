#!/usr/bin/env python3
"""Escucha el socket de depuracion de la PS4 (4646) y guarda la salida.

La salida estandar y de error de la aplicacion homebrew (stdout/stderr) va a
ese socket, que es la forma de ver los mensajes de Kodi (por ejemplo
"terminate called after throwing an instance of ...").

El puerto 4646 solo esta abierto mientras hay una aplicacion homebrew
funcionando, asi que el script reintenta la conexion hasta que se abre.

Uso:
  ./scripts/console-debug-socket.py [segundos] [-f]
"""

from __future__ import annotations

import os
import socket
import sys
import time

CONSOLE = os.environ.get("CONSOLE", "192.0.2.11")
PORT = int(os.environ.get("DEBUG_PORT", "4646"))


def main() -> int:
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 120
    save = "-f" in sys.argv[2:]

    deadline = time.time() + seconds
    out = b""
    sock = None
    print(f"escuchando {CONSOLE}:{PORT} durante {seconds}s (lanza la app en la consola)")
    while time.time() < deadline:
        try:
            sock = socket.create_connection((CONSOLE, PORT), timeout=5)
            print("conectado")
            break
        except OSError:
            time.sleep(0.5)

    if sock is None:
        print("el socket no se abrio (la app no ha arrancado?)")
        return 1

    sock.settimeout(5)
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        out += chunk
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()

    sock.close()
    text = out.decode("utf-8", "replace")
    if not text.strip():
        print("(vacio: el socket abrio pero la app no escribio nada)")
    if save:
        os.makedirs("build", exist_ok=True)
        path = os.path.join("build", f"debug-socket-{time.strftime('%H%M%S')}.log")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"guardado en {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
