#!/usr/bin/env bash
#
# privacy-check.sh — abort if the port or PKG staging contains user data,
# credentials, or a concrete console configuration.
#
# Usage: ./scripts/privacy-check.sh [path...]
# By default it checks port/, packaging/ and build/core-stage/.
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATHS=("$@")
if [[ ${#PATHS[@]} -eq 0 ]]; then
  for candidate in "$ROOT_DIR/port" "$ROOT_DIR/packaging" "$ROOT_DIR/build/core-stage"; do
    [[ -e "$candidate" ]] && PATHS+=("$candidate")
  done
fi

[[ ${#PATHS[@]} -gt 0 ]] || { echo "privacy: nothing to check"; exit 0; }

fail=0
report() {
  printf 'privacy: %s\n' "$*" >&2
  fail=1
}

# 1. Directories and files that must never ship in the PKG.
while IFS= read -r -d '' item; do
  report "private path: ${item#"$ROOT_DIR"/}"
done < <(find "${PATHS[@]}" \( \
  -type d \( -name userdata -o -name addon_data -o -name .ssh -o -name .gnupg \) -o \
  -type f \( -iname '*.m3u' -o -iname '*.m3u8' -o -iname '*.db' -o \
             -iname '*.sqlite' -o -iname '*.sqlite3' -o -iname '*.log' -o \
             -iname '*.key' -o -iname '*.p12' -o -iname '*.pfx' -o \
             -iname 'id_rsa' -o -iname 'id_ed25519' -o -iname '*.kdbx' \) \
  \) -print0 2>/dev/null)

# 2. Private IPs or MAC addresses in the code that is packaged (not docs).
for path in "$ROOT_DIR/port" "$ROOT_DIR/packaging"; do
  [[ -d "$path" ]] || continue
  if grep -rInE \
      '192\.168\.[0-9]+\.[0-9]+|10\.[0-9]+\.[0-9]+\.[0-9]+|172\.(1[6-9]|2[0-9]|3[01])\.[0-9]+\.[0-9]+|([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}' \
      "$path" >/dev/null 2>&1; then
    report "private IP/MAC found under ${path#"$ROOT_DIR"/}"
  fi
done

if [[ $fail -ne 0 ]]; then
  echo "privacy: FAILED — remove those data before packaging" >&2
  exit 1
fi

echo "privacy: OK (${PATHS[*]##*/})"
