#!/usr/bin/env python3
"""Remove stale entries from a CMake cache file.

Entries whose value points at a host library have to be dropped when the
cross build starts using its own copy of that library, otherwise CMake keeps
the cached result.  Removing an entry leaves its comment line behind, which
the cache parser rejects, so those are dropped as well.
"""
import re
import sys

path = sys.argv[1]
patterns = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None

with open(path) as f:
    lines = f.readlines()

out = []
removed = []
i = 0
while i < len(lines):
    line = lines[i]
    if patterns and patterns.match(line):
        removed.append(line.strip())
        # drop the comment line that documents the entry
        if out and out[-1].startswith("//"):
            out.pop()
        i += 1
        continue
    out.append(line)
    i += 1

# A comment line that is not followed by an entry is left over from a removed
# one and makes the cache unreadable.
cleaned = []
i = 0
while i < len(out):
    if out[i].startswith("//") and i + 1 < len(out) and not out[i + 1].strip():
        i += 1
        continue
    cleaned.append(out[i])
    i += 1

with open(path, "w") as f:
    f.writelines(cleaned)

print(f"removed {len(removed)} stale cache entries")
for entry in removed:
    print("  ", entry)
