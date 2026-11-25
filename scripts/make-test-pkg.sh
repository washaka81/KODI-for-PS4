#!/usr/bin/env bash
#
# make-test-pkg.sh — Build a small test package for the PS4.
#
# Usage:
#   ./scripts/make-test-pkg.sh <elf> <eboot> <content-id> <title-id> <title> <pkg-out>
#
# The content id must be exactly 36 characters, the PkgTool aborts otherwise.
#
set -euo pipefail
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="$ROOT_DIR/tools/ssl-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
PS4SDK="${OO_PS4_TOOLCHAIN:-${PS4SDK:-${HOME}/ps4sdk/OpenOrbis/PS4Toolchain}}"
TOOLS="$PS4SDK/bin/linux"

ELF="$1"; EBOOT="$2"; CID="$3"; TID="$4"; TITLE="$5"; PKG="$6"

if [[ ${#CID} -ne 36 ]]; then
  echo "ERROR: el content id debe tener 36 caracteres (tiene ${#CID}): $CID" >&2
  exit 2
fi

STAGE="$(mktemp -d)"
export OO_PS4_TOOLCHAIN="$PS4SDK"
"$TOOLS/create-fself" -in="$ELF" -out="$STAGE/x.oelf" -eboot="$EBOOT" \
  -paid=0x3800000000000035 \
  -authinfo="000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000" >/dev/null

mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module"
cp "$EBOOT" "$STAGE/eboot.bin"
cp "$ROOT_DIR/packaging/icon0.png" "$STAGE/sce_sys/icon0.png"

SDK_SAMPLE_DIR="$PS4SDK/samples/graphics"
copy_sample_file() {
  local rel="$1" dest="$2"
  if [[ -f "$SDK_SAMPLE_DIR/$rel" ]]; then cp "$SDK_SAMPLE_DIR/$rel" "$dest"; return; fi
  local sample_pkg tmp
  sample_pkg="$(find "$PS4SDK/samples" -name '*.pkg' | head -1)"
  [[ -n "$sample_pkg" ]] || { echo "ERROR: no OpenOrbis sample package found for $rel" >&2; exit 1; }
  tmp="$(mktemp -d)"
  "$TOOLS/PkgTool.Core" pkg_extract "$sample_pkg" "$tmp" >/dev/null
  cp "$tmp/uroot/$rel" "$dest"; rm -rf "$tmp"
}
copy_sample_file "sce_sys/keystone" "$STAGE/sce_sys/keystone"
copy_sample_file "sce_sys/about/right.sprx" "$STAGE/sce_sys/about/right.sprx"
copy_sample_file "sce_module/libc.prx" "$STAGE/sce_module/libc.prx"
copy_sample_file "sce_module/libSceFios2.prx" "$STAGE/sce_module/libSceFios2.prx"

SFO="$STAGE/sce_sys/param.sfo"
"$TOOLS/PkgTool.Core" sfo_new "$SFO"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_TYPE --type Integer --maxsize 4 --value 1
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_VER --type Utf8 --maxsize 8 --value "01.00"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" ATTRIBUTE --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CATEGORY --type Utf8 --maxsize 4 --value gde
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CONTENT_ID --type Utf8 --maxsize 48 --value "$CID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" SYSTEM_VER --type Integer --maxsize 4 --value 1020
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE --type Utf8 --maxsize 128 --value "$TITLE"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE_ID --type Utf8 --maxsize 12 --value "$TID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" FORMAT --type Utf8 --maxsize 4 --value obs
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" VERSION --type Utf8 --maxsize 8 --value "01.00"

"$TOOLS/create-gp4" -out="$STAGE/t.gp4" --content-id="$CID" \
  --files "eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/keystone sce_module/libc.prx sce_module/libSceFios2.prx" >/dev/null
"$TOOLS/PkgTool.Core" pkg_build "$STAGE/t.gp4" "$(dirname "$PKG")" >/dev/null
rm -rf "$STAGE"
ls -la "$PKG"
