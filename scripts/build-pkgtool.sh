#!/usr/bin/env bash
set -euo pipefail
SRC_DIR="${SRC_DIR:-tools/LibOrbisPkg-src}"
if ! command -v mono >/dev/null 2>&1; then
  echo "ERROR: mono is required" >&2
  exit 1
fi
cd "$SRC_DIR"
if command -v msbuild >/dev/null 2>&1; then
  msbuild LibOrbisPkg.sln /p:Configuration=Debug
elif command -v xbuild >/dev/null 2>&1; then
  xbuild LibOrbisPkg.sln /p:Configuration=Debug
else
  echo "ERROR: msbuild/xbuild not found (part of mono)" >&2
  exit 1
fi
echo "PkgTool built at PkgTool/bin/Debug/PkgTool.exe"
```, path=scripts/build-pkgtool.sh