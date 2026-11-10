#!/usr/bin/env python3
"""Patch the OpenOrbis libc++ static library: ETIMEDOUT check.

`std::condition_variable::__do_timed_wait` in the prebuilt libc++.a compares
pthread_cond_timedwait()'s return value against the Linux ETIMEDOUT (110).
The PS4 kernel (FreeBSD) returns 60, so every timed wait that times out
throws `condition_variable timed_wait failed` and Kodi aborts (seen as
"condition_variable timed_wait failed: Operation timed out" right after
Initialize()).

This script rewrites that one comparison (0x6e -> 0x3c) inside the
condition_variable.cpp.o member of libc++.a.  Idempotent.

Usage: ./scripts/patch-libcxx-etimedout.py [--toolchain DIR]
"""

import argparse
import os
import subprocess
import sys
import tempfile

PATTERN_OLD = bytes([0x83, 0x7D, 0x94, 0x6E])  # cmpl $110,-0x6c(%rbp)
PATTERN_NEW = bytes([0x83, 0x7D, 0x94, 0x3C])  # cmpl $60,-0x6c(%rbp)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--toolchain",
        default=os.environ.get("OO_PS4_TOOLCHAIN", "/opt/ps4/sdk"),
    )
    args = ap.parse_args()

    lib = os.path.join(args.toolchain, "lib", "libc++.a")
    if not os.path.isfile(lib):
        print(f"ERROR: {lib} not found", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        member = "condition_variable.cpp.o"
        subprocess.run(["ar", "x", lib, member], cwd=tmp, check=True)
        path = os.path.join(tmp, member)
        data = bytearray(open(path, "rb").read())

        if data.count(PATTERN_NEW) == 1 and data.count(PATTERN_OLD) == 0:
            print("libc++ already patched")
            return 0
        if data.count(PATTERN_OLD) != 1:
            print(
                f"ERROR: expected exactly one {PATTERN_OLD.hex()} in {member}, "
                f"found {data.count(PATTERN_OLD)}",
                file=sys.stderr,
            )
            return 1

        data = data.replace(PATTERN_OLD, PATTERN_NEW)
        open(path, "wb").write(data)
        backup = lib + ".orig"
        if not os.path.exists(backup):
            subprocess.run(["cp", "-p", lib, backup], check=True)
        subprocess.run(["ar", "r", lib, member], cwd=tmp, check=True)

    print(f"patched ETIMEDOUT check in {lib}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
