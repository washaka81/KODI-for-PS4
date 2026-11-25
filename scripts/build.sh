#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PS4SDK="${OO_PS4_TOOLCHAIN:-${PS4SDK:-}}"
if [[ -z "$PS4SDK" ]]; then
  for candidate in \
    ${HOME}/ps4sdk/OpenOrbis/PS4Toolchain \
    /opt/ps4/sdk \
    /opt/ps4/oosdk ; do
    if [[ -d "$candidate/bin/linux" ]]; then PS4SDK="$candidate"; break; fi
  done
fi

if [[ -z "$PS4SDK" || ! -d "$PS4SDK" ]]; then
  echo "ERROR: PS4 SDK not found. Set OO_PS4_TOOLCHAIN or PS4SDK." >&2
  exit 1
fi

TARGET="x86_64-pc-freebsd12-elf"
BUILD_DIR="$ROOT_DIR/build"
OBJ_DIR="$BUILD_DIR/ps4/obj"
ELF_PATH="$BUILD_DIR/kodi-ps4.elf"
EBOOT_PATH="$BUILD_DIR/eboot.bin"
mkdir -p "$OBJ_DIR" "$BUILD_DIR"
# Drop objects of sources that are no longer part of the build.
rm -f "$OBJ_DIR"/*.o

echo "Using PS4 SDK: $PS4SDK"
echo "Compiling sources..."
# Keep this list in sync with src/platform/ps4/CMakeLists.txt: the library
# sources plus the main_stub entry point.  MainPiglet.cpp is the legacy
# prototype and defines a second main().
SOURCES=(
  "$ROOT_DIR/src/platform/ps4/ApplicationBase.cpp"
  "$ROOT_DIR/src/platform/ps4/CAESinkPS4.cpp"
  "$ROOT_DIR/src/platform/ps4/PS4AudioSink.cpp"
  "$ROOT_DIR/src/platform/ps4/PS4Input.cpp"
  "$ROOT_DIR/src/platform/ps4/PS4RendererGLES2.cpp"
  "$ROOT_DIR/src/platform/ps4/PlatformPS4.cpp"
  "$ROOT_DIR/src/platform/ps4/main_stub.cpp"
)
for src in "${SOURCES[@]}"; do
  rel="$(basename "$src" .cpp).o"
  echo "  CC  $rel"
  clang --target="$TARGET" -fPIC -funwind-tables -c \
    -isysroot "$PS4SDK" \
    -isystem "$PS4SDK/include" \
    -isystem "$PS4SDK/include" \
    -isystem "$PS4SDK/include/c++/v1" \
    -isystem "$PS4SDK/samples/_common" \
    -isystem "$PS4SDK/samples/_common/ootk" \
    -DPS4 -D__ORBIS__ -DORBIS \
    "$src" -o "$OBJ_DIR/$rel"
done

echo "Linking ELF..."
ld.lld "$OBJ_DIR"/*.o -o "$ELF_PATH" \
  -m elf_x86_64 -pie --script "$PS4SDK/link.x" --eh-frame-hdr \
  -L"$PS4SDK/lib" \
  -lc -lkernel -lc++ \
   -lSceUserService -lSceSystemService -lSceSysmodule  -lSceShellCoreUtil -lScePrecompiledShaders -lSceRtc -lScePad \
  -lSceAudioOut -lSceVideoOut -lScePigletv2VSH \
  -lSceVideodec2 -lSceGnmDriver \
  "$PS4SDK/lib/crt1.o"

echo "Creating SELF (eboot.bin)..."
export OO_PS4_TOOLCHAIN="$PS4SDK"
"$PS4SDK/bin/linux/create-fself" \
  -in="$ELF_PATH" \
  -out="$BUILD_DIR/kodi-ps4.oelf" \
  -eboot="$EBOOT_PATH" \
  -paid=0x3800000000000035 \
  -authinfo="000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

echo "Build successful:"
echo "  ELF : $ELF_PATH"
echo "  EBT : $EBOOT_PATH"
