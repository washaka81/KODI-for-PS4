#!/usr/bin/env bash
set -euo pipefail
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="$ROOT_DIR/tools/ssl-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
PS4SDK="${OO_PS4_TOOLCHAIN:-${PS4SDK:-}}"
if [[ -z "$PS4SDK" ]]; then
  for candidate in \
    ${HOME}/ps4sdk/OpenOrbis/PS4Toolchain \
    /opt/ps4/sdk \
    /opt/ps4/oosdk ; do
    if [[ -d "$candidate/bin/linux" ]]; then PS4SDK="$candidate"; break; fi
  done
fi
if [[ -z "$PS4SDK" || ! -d "$PS4SDK/bin/linux" ]]; then
  echo "ERROR: PS4 SDK not found." >&2
  exit 1
fi

TOOLS="$PS4SDK/bin/linux"
PKG_STAGE="${PKG_STAGE:-$ROOT_DIR/build/pkg-stage}"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT_DIR/dist}"
EBOOT_PATH="${EBOOT_PATH:-$ROOT_DIR/build/eboot.bin}"
CONTENT_ID="${CONTENT_ID:-IV0000-BREW99999_00-KODIPS4000000000}"
TITLE_ID="${TITLE_ID:-BREW99999}"
TITLE="${TITLE:-KODI}"
VERSION="${VERSION:-01.00}"
ICON_PATH="${ICON_PATH:-$ROOT_DIR/packaging/icon0.png}"

if [[ ! -f "$EBOOT_PATH" ]]; then
  echo "ERROR: eboot not found at $EBOOT_PATH; run scripts/build.sh first." >&2
  exit 1
fi

rm -rf "$PKG_STAGE"
mkdir -p "$PKG_STAGE/sce_sys/about" "$PKG_STAGE/sce_module" "$OUTPUT_DIR"
cp "$EBOOT_PATH" "$PKG_STAGE/eboot.bin"
cp "$ICON_PATH" "$PKG_STAGE/sce_sys/icon0.png"

# Required system helper files from the OpenOrbis samples.  They ship inside
# the sample packages, so extract them from there when the SDK tree does not
# provide them.
SDK_SAMPLE_DIR="${SDK_SAMPLE_DIR:-$PS4SDK/samples/graphics}"
copy_sample_file() {
  local rel="$1" dest="$2"
  if [[ -f "$SDK_SAMPLE_DIR/$rel" ]]; then
    cp "$SDK_SAMPLE_DIR/$rel" "$dest"
    return
  fi
  local sample_pkg
  sample_pkg="$(find "$PS4SDK/samples" -name '*.pkg' | head -1)"
  [[ -n "$sample_pkg" ]] || { echo "ERROR: no OpenOrbis sample package found for $rel" >&2; exit 1; }
  local tmp
  tmp="$(mktemp -d)"
  "$TOOLS/PkgTool.Core" pkg_extract "$sample_pkg" "$tmp" >/dev/null
  cp "$tmp/uroot/$rel" "$dest"
  rm -rf "$tmp"
}

copy_sample_file "sce_sys/keystone" "$PKG_STAGE/sce_sys/keystone"
copy_sample_file "sce_sys/about/right.sprx" "$PKG_STAGE/sce_sys/about/right.sprx"
copy_sample_file "sce_module/libc.prx" "$PKG_STAGE/sce_module/libc.prx"
copy_sample_file "sce_module/libSceFios2.prx" "$PKG_STAGE/sce_module/libSceFios2.prx"

SFO="$PKG_STAGE/sce_sys/param.sfo"
"$TOOLS/PkgTool.Core" sfo_new "$SFO"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_TYPE --type Integer --maxsize 4 --value 1
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" ATTRIBUTE --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CATEGORY --type Utf8 --maxsize 4 --value gde
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" SYSTEM_VER --type Integer --maxsize 4 --value 1020
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE --type Utf8 --maxsize 128 --value "$TITLE"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" FORMAT --type Utf8 --maxsize 4 --value obs
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" VERSION --type Utf8 --maxsize 8 --value "$VERSION"

GP4="$PKG_STAGE/kodi-ps4.gp4"
export OO_PS4_TOOLCHAIN="$PS4SDK"
"$TOOLS/create-gp4" -out="$GP4" --content-id="$CONTENT_ID" \
  --files "eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/keystone sce_module/libc.prx sce_module/libSceFios2.prx"

echo "Building fake PKG..."
"$TOOLS/PkgTool.Core" pkg_build "$GP4" "$OUTPUT_DIR"

PKG_FILE="$OUTPUT_DIR/${CONTENT_ID}.pkg"
if [[ ! -f "$PKG_FILE" ]]; then
  echo "ERROR: package not created" >&2
  exit 1
fi

echo "PKG built: $PKG_FILE"
echo "Upload command:"
echo "  curl --ftp-create-dirs -T \"$PKG_FILE\" ftp://192.0.2.11:2121/data/kodi-ps4/$(basename "$PKG_FILE")"
sha256sum "$PKG_FILE"
