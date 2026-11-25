#!/usr/bin/env python3
"""Kodi PS4 database maintenance: persistence by default, purge on demand.

The old workflow deleted every *.db before each launch to avoid the PS4
fstat() truncation bug.  The SQLite VFS now tracks the real file size, so
that is no longer necessary: databases and settings must persist.

Usage:
    ./scripts/console-clean-dbs.py                 # status only (no changes)
    ./scripts/console-clean-dbs.py backup          # download Database/ to build/
    ./scripts/console-clean-dbs.py purge --yes     # backup first, then delete
    ./scripts/console-clean-dbs.py unlock --force  # remove stale locks only
    ./scripts/console-clean-dbs.py restore DIR     # upload a previous backup

`purge` without --yes only prints what it would remove.
"""
import importlib.util
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
    "console_logs", ROOT / "scripts" / "console-logs.py")
console_logs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(console_logs)

BASE = "/data/kodi-ps4/.kodi/userdata/Database"
BACKUP_ROOT = ROOT / "build"


def _is_db_file(name: str) -> bool:
    return name.endswith((".db", ".db-journal", ".db-wal"))


def _rmtree(console, path: str) -> None:
    for name, _size, _stamp, is_dir in console.listdir(path):
        sub = f"{path}/{name}"
        if is_dir:
            _rmtree(console, sub)
        else:
            console.delete(sub)
    try:
        console.ftp.rmd(path)
    except Exception:
        pass


def status(console) -> int:
    entries = console.listdir(BASE)
    if not entries:
        print(f"{BASE}: no existe o está vacío")
        return 0
    print(f"{BASE}:")
    bad = 0
    for name, size, stamp, is_dir in entries:
        if is_dir:
            print(f"  [dir] {name}")
            continue
        mark = ""
        if _is_db_file(name) and size in (0, 128):
            mark = "  <-- POSIBLE TRUNCADO"
            bad += 1
        print(f"  {size:>9} bytes  {stamp}  {name}{mark}")
    if any(name.endswith(".lock") for name, _s, _t, _d in entries):
        print("\nAviso: hay .lock (Kodi en ejecución o locks huérfanos de un "
              "cierre duro). Ciérralo antes de backup/purge y usa "
              "'unlock --force' para quitar solo los locks.")
    if bad:
        print(f"\nAviso: {bad} fichero(s) con tamaño sospechoso. Si Kodi da "
              "SQLITE_CORRUPT, usa: purge --yes (hace backup antes).")
    else:
        print("\nDBs presentes y con tamaño válido: se conservan entre arranques.")
    return 0


def backup(console) -> tuple[pathlib.Path, list[str]]:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = BACKUP_ROOT / f"db-backup-{stamp}"
    out.mkdir(parents=True, exist_ok=True)
    total = 0
    failed: list[str] = []
    for name, _size, _stamp, is_dir in console.listdir(BASE):
        if is_dir or not _is_db_file(name):
            continue
        data = console.get(f"{BASE}/{name}")
        if data is None:
            print(f"  no se pudo descargar {name} (¿Kodi abierto?)",
                  file=sys.stderr)
            failed.append(name)
            continue
        (out / name).write_bytes(data)
        total += len(data)
        print(f"  {name}: {len(data)} bytes")
    print(f"backup: {out} ({total} bytes)")
    if failed:
        print("backup incompleto: " + ", ".join(failed), file=sys.stderr)
    return out, failed


def purge(console, confirm: bool) -> int:
    entries = console.listdir(BASE)
    targets = [(n, s, d) for n, s, _stamp, d in entries
               if _is_db_file(n) or (d and n.endswith(".lock"))]
    if not targets:
        print("No hay DBs ni locks que purgar.")
        return 0
    for name, size, is_dir in targets:
        kind = "dir" if is_dir else f"{size} bytes"
        print(f"  {'borraría' if not confirm else 'borrando'} {name} ({kind})")
    if not confirm:
        print("\nNo se ha borrado nada. Repite con: purge --yes")
        return 0

    out, failed = backup(console)
    if failed and "--force" not in sys.argv[2:]:
        print("\nNo se purga: el backup quedó incompleto. Cierra Kodi y "
              "reintenta, o usa --force si aceptas perder esas DBs.",
              file=sys.stderr)
        return 1
    for name, _size, is_dir in targets:
        path = f"{BASE}/{name}"
        if is_dir:
            _rmtree(console, path)
        else:
            console.delete(path)
    print(f"purga completada; copia de seguridad en {out}")
    return 0


def unlock(console, force: bool) -> int:
    """Remove only the .lock entries; database files are untouched.

    SQLite's unix-dotfile VFS creates a directory named <db>.lock, but accept
    regular files too so an older/different VFS is also cleaned.
    """
    entries = console.listdir(BASE)
    locks = [(name, is_dir) for name, _size, _stamp, is_dir in entries
             if name.endswith(".lock")]
    if not locks:
        print("No hay locks en la base de datos.")
        return 0
    for name, is_dir in locks:
        print(f"  lock: {name} ({'dir' if is_dir else 'file'})")
    if not force:
        print("\nKodi puede estar en ejecución. Ciérralo y repite con: "
              "unlock --force")
        return 1
    for name, is_dir in locks:
        path = f"{BASE}/{name}"
        if is_dir:
            _rmtree(console, path)
        else:
            console.delete(path)
        print(f"  eliminado {name}")
    print("locks eliminados; las DBs no se han tocado.")
    return 0


def restore(console, src: str) -> int:
    src_path = pathlib.Path(src)
    if not src_path.is_dir():
        print(f"no existe el directorio {src}", file=sys.stderr)
        return 2
    count = 0
    for path in sorted(src_path.iterdir()):
        if not path.is_file():
            continue
        with path.open("rb") as fh:
            console.ftp.storbinary(f"STOR {BASE}/{path.name}", fh)
        print(f"  restaurado {path.name} ({path.stat().st_size} bytes)")
        count += 1
    print(f"restaurados {count} fichero(s)")
    return 0


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "status"
    console = console_logs.Console()
    try:
        if mode == "status":
            return status(console)
        if mode == "backup":
            _out, failed = backup(console)
            return 1 if failed else 0
        if mode == "purge":
            return purge(console, "--yes" in sys.argv[2:])
        if mode == "unlock":
            return unlock(console, "--force" in sys.argv[2:])
        if mode == "restore":
            if len(sys.argv) < 3:
                print("uso: console-clean-dbs.py restore <directorio>",
                      file=sys.stderr)
                return 2
            return restore(console, sys.argv[2])
        print(__doc__)
        return 2
    finally:
        console.close()


if __name__ == "__main__":
    sys.exit(main())
