#!/usr/bin/env bash
#
# package-core.sh — Package the Kodi core binary for the PS4.
#
# The core needs its resources (skins, system files, media) next to the
# executable, so the package carries them under media/kodi.  The application
# sets KODI_HOME to that directory and keeps the profile in $HOME.
#
set -euo pipefail

export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -d "$ROOT_DIR/tools/ssl-compat" ]]; then
  export LD_LIBRARY_PATH="$ROOT_DIR/tools/ssl-compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
KODI_DIR="$ROOT_DIR/src/kodi"
PS4SDK="${OO_PS4_TOOLCHAIN:-${PS4SDK:-}}"
if [[ -z "$PS4SDK" ]]; then
  for candidate in \
    /opt/ps4/sdk \
    /opt/ps4/oosdk ; do
    if [[ -d "$candidate/bin/linux" ]]; then PS4SDK="$candidate"; break; fi
  done
fi
[[ -d "$PS4SDK/bin/linux" ]] || { echo "ERROR: PS4 SDK not found." >&2; exit 1; }

TOOLS="$PS4SDK/bin/linux"
STAGE="$ROOT_DIR/build/core-stage"
OUTPUT_DIR="$ROOT_DIR/dist"
CORE_ELF="${CORE_ELF:-$ROOT_DIR/build/kodi-core.elf}"
EBOOT="${EBOOT:-$ROOT_DIR/build/kodi-core-eboot.bin}"
CONTENT_ID="${CONTENT_ID:-IV0000-BREW99998_00-KODICORE00000000}"
TITLE_ID="${TITLE_ID:-BREW99998}"
TITLE="${TITLE:-KODI}"
VERSION="${VERSION:-01.00}"
CATEGORY="${CATEGORY:-gd}"
SYSTEM_VER="${SYSTEM_VER:-0}"
ICON_PATH="${ICON_PATH:-$ROOT_DIR/packaging/icon0.png}"

[[ -f "$CORE_ELF" ]] || { echo "ERROR: $CORE_ELF not found." >&2; exit 1; }
[[ -f "$EBOOT" ]] || { echo "ERROR: $EBOOT not found." >&2; exit 1; }
# A truncated SELF (for example, if the disk fills while generating it) would start
# with an error unrelated to what is being tested.
elf_size=$(stat -c%s "$CORE_ELF")
eboot_size=$(stat -c%s "$EBOOT")
if (( eboot_size < elf_size / 2 )); then
  echo "ERROR: eboot ($eboot_size bytes) looks truncated for an ELF of $elf_size bytes" >&2
  exit 1
fi
[[ ${#CONTENT_ID} -eq 36 ]] || { echo "ERROR: content id must be 36 characters (has ${#CONTENT_ID}): $CONTENT_ID" >&2; exit 2; }
[[ -f "$ICON_PATH" ]] || { echo "ERROR: $ICON_PATH not found." >&2; exit 1; }

echo "Staging the package in $STAGE ..."
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module" "$STAGE/media/kodi" "$OUTPUT_DIR"

cp "$EBOOT" "$STAGE/eboot.bin"
cp "$ICON_PATH" "$STAGE/sce_sys/icon0.png"

# Kodi resources: the skin, the system files and the media.
if [[ "${WITH_MEDIA:-1}" == "1" ]]; then
  cp -a "$KODI_DIR/addons/skin.estuary" "$STAGE/media/kodi/addons_tmp"
  mkdir -p "$STAGE/media/kodi/addons"
  mv "$STAGE/media/kodi/addons_tmp" "$STAGE/media/kodi/addons/skin.estuary"
  cp -a "$KODI_DIR/system" "$STAGE/media/kodi/system"
  cp -a "$KODI_DIR/media" "$STAGE/media/kodi/media"
  # The addon.xml definitions cmake generates in the build tree: the manifest
  # check in CAddonMgr::Init requires every mandatory addon to be findable.
  KODI_BUILD="${KODI_BUILD:-${HOME}/kodi-build-ps4}"
  if [[ -d "$KODI_BUILD/addons" ]]; then
    cp -a "$KODI_BUILD/addons/." "$STAGE/media/kodi/addons/"
  else
    echo "ERROR: $KODI_BUILD/addons not found (compila el core primero)" >&2
    exit 1
  fi
  # Trim heavy addon payloads that are not needed to boot into the GUI.
  # addon.xml stays so the mandatory manifest check keeps working; missing
  # content can be reinstalled from the boxed repository.xbmc.org (which is
  # kept).  Estuary, the default game controller and the repository are
  # never trimmed.
  if [[ "${PRUNE_ADDONS:-0}" == "1" ]]; then
    PRUNE_LIST="webinterface.default resource.images.weathericons.default
      service.xbmc.versioncheck game.controller.snes game.controller.keyboard
      game.controller.mouse metadata.themoviedb.org.python
      metadata.tvshows.themoviedb.org.python metadata.generic.artists
      metadata.generic.albums metadata.artists.universal
      metadata.album.universal audioencoder.kodi.builtin.wma
      audioencoder.kodi.builtin.aac"
    for addon in $PRUNE_LIST; do
      dir="$STAGE/media/kodi/addons/$addon"
      [[ -d "$dir" ]] || continue
      find "$dir" -type f ! -name addon.xml -delete 2>/dev/null || true
      find "$dir" -mindepth 1 -type d -empty -delete 2>/dev/null || true
    done
    echo "Addons trimmed (addon.xml kept); reinstall from repository.xbmc.org"
  fi

  # Clear shader used by the glshim engine (sceGnm clear utility).
  mkdir -p "$STAGE/assets/misc"
  if ! cp "$ROOT_DIR/tools/gnm-engine/assets/misc/clear.frag.sb" "$STAGE/assets/misc/"; then
    echo "ERROR: clear.frag.sb missing; glshim cannot clear the screen" >&2
    exit 1
  fi

  # Precompiled shader binaries (Piglet container + PSSL reflection, keyed by
  # the djb2 hash of the final GLSL source).
  if [[ -d "$ROOT_DIR/build/shaderbin" ]]; then
    mkdir -p "$STAGE/media/kodi/system/shaders/bin"
    cp -a "$ROOT_DIR/build/shaderbin/." "$STAGE/media/kodi/system/shaders/bin/"
    echo "Shader blobs: $(ls "$STAGE/media/kodi/system/shaders/bin" | wc -l)"
  else
    echo "WARNING: build/shaderbin not found; the GUI will stay black" >&2
  fi
fi

# System helper files from the OpenOrbis samples.
SDK_SAMPLE_DIR="${SDK_SAMPLE_DIR:-$PS4SDK/samples/graphics}"
copy_sample_file() {
  local rel="$1" dest="$2"
  if [[ -f "$SDK_SAMPLE_DIR/$rel" ]]; then
    cp "$SDK_SAMPLE_DIR/$rel" "$dest"
    return
  fi
  local sample_pkg tmp
  sample_pkg="$(find "$PS4SDK/samples" -name '*.pkg' | head -1)"
  [[ -n "$sample_pkg" ]] || { echo "ERROR: no OpenOrbis sample package found for $rel" >&2; exit 1; }
  tmp="$(mktemp -d)"
  "$TOOLS/PkgTool.Core" pkg_extract "$sample_pkg" "$tmp" >/dev/null
  cp "$tmp/uroot/$rel" "$dest"
  rm -rf "$tmp"
}
copy_sample_file "sce_sys/keystone" "$STAGE/sce_sys/keystone"
copy_sample_file "sce_sys/about/right.sprx" "$STAGE/sce_sys/about/right.sprx"
copy_sample_file "sce_module/libc.prx" "$STAGE/sce_module/libc.prx"
copy_sample_file "sce_module/libSceFios2.prx" "$STAGE/sce_module/libSceFios2.prx"

SFO="$STAGE/sce_sys/param.sfo"
"$TOOLS/PkgTool.Core" sfo_new "$SFO"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_TYPE --type Integer --maxsize 4 --value 1
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
# ATTRIBUTE bit 29 (0x20000000) is the "supports HDR" flag the PS4 system
# reads from param.sfo.  Without it the console refuses to expose the
# BT2020/PQ capability and HDR stays unavailable to the app.
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" ATTRIBUTE --type Integer --maxsize 4 --value 536870912
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CATEGORY --type Utf8 --maxsize 4 --value "$CATEGORY"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" SYSTEM_VER --type Integer --maxsize 4 --value "$SYSTEM_VER"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE --type Utf8 --maxsize 128 --value "$TITLE"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" VERSION --type Utf8 --maxsize 8 --value "$VERSION"

GP4="$STAGE/kodi-core.gp4"
export OO_PS4_TOOLCHAIN="$PS4SDK"
# The SDK tool only accepts a flat list of files, the Kodi resources need
# the whole directory tree.
python3 "$ROOT_DIR/scripts/make-gp4.py" "$STAGE" "$GP4" "$CONTENT_ID"

echo "Building the fake PKG ..."
"$TOOLS/PkgTool.Core" pkg_build "$GP4" "$OUTPUT_DIR"

PKG_FILE="$OUTPUT_DIR/${CONTENT_ID}.pkg"
[[ -f "$PKG_FILE" ]] || { echo "ERROR: package not created" >&2; exit 1; }

echo "PKG built: $PKG_FILE"
echo "Upload command:"
echo "  Upload $PKG_FILE to your console and install it."
sha256sum "$PKG_FILE"
