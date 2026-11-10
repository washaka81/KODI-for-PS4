## ps4sdk-direct.cmake - OpenOrbis 0.5.4 direct linker

set(CMAKE_SYSTEM_NAME Orbis)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSTEM_VERSION 9)

if(NOT DEFINED PS4SDK)
  if(DEFINED ENV{PS4SDK})
    set(PS4SDK "$ENV{PS4SDK}")
  elseif(DEFINED ENV{OO_PS4_TOOLCHAIN})
    set(PS4SDK "$ENV{OO_PS4_TOOLCHAIN}")
  else()
    set(PS4SDK "/opt/ps4/sdk")
  endif()
endif()

file(TO_CMAKE_PATH "${PS4SDK}" PS4SDK)

set(PS4_LIB_DIR "${PS4SDK}/lib")
set(CMAKE_FIND_ROOT_PATH "${PS4SDK}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-freebsd12-elf)
# The compiler ABI probe runs during project(), before Kodi's
# CompilerSettings.cmake can request C++20.  The forced-include std::ranges
# polyfill needs concepts, so the probe must already be C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_LINKER /usr/bin/ld.lld)
set(CMAKE_AR /usr/bin/ar)
set(CMAKE_RANLIB /usr/bin/ranlib)

# Clang's resource headers provide stdatomic.h and other compiler headers that
# the PS4 sysroot does not ship.
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
  OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)

set(CMAKE_C_FLAGS "--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -fdata-sections -ffunction-sections -nostdinc -isysroot ${PS4SDK} -isystem ${PS4SDK}/include -isystem ${PS4SDK}/include/c++/v1 -isystem ${CLANG_RESOURCE_DIR}/include -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D__GNUC__=13 -Wno-double-promotion -Wno-macro-redefined")
# Extra defines for a specific target build, set through the cache by the
# build scripts (eg the PS4 SDK headers lack some socket constants).
if(PS4_EXTRA_C_FLAGS)
  string(APPEND CMAKE_C_FLAGS " ${PS4_EXTRA_C_FLAGS}")
endif()
set(CMAKE_CXX_FLAGS "--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -fdata-sections -ffunction-sections -nostdinc -isysroot ${PS4SDK} -isystem ${PS4SDK}/include -isystem ${PS4SDK}/include/c++/v1 -isystem ${CLANG_RESOURCE_DIR}/include -include ranges -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D__GNUC__=13 -D__FreeBSD_version=900000 -Wno-double-promotion -Wno-macro-redefined")
# The compiler driver adds a PT_INTERP segment with the interpreter path,
# but the SDK linker script already stores it at the start of .text.  The
# loader only expects the one in .text, and the extra segment makes the
# application fail to start.
set(CMAKE_EXE_LINKER_FLAGS "-nostdlib -fuse-ld=lld -pie -Wl,--gc-sections -Wl,--no-dynamic-linker -Wl,-m,elf_x86_64 -Wl,--script,${PS4SDK}/link.x -Wl,--eh-frame-hdr -L${PS4_LIB_DIR} -lc -lkernel -lc++ ${PS4_LIB_DIR}/crt1.o ")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

# Copy defeats CMake implicit-include dedup so PS4SDK headers
# precede host -isystem entries (wchar.h shadow fix).
include_directories(BEFORE SYSTEM ${CLANG_RESOURCE_DIR}/include)
include_directories(BEFORE SYSTEM /tmp/ps4sdk-include-copy)
include_directories(BEFORE SYSTEM ${PS4SDK}/include/orbis ${PS4SDK}/include/c++/v1)
# The GUI renders through our own GLES2/EGL shim over sceGnm instead of
# Piglet.  Static archives go before the system libraries that satisfy them.
set(GLSHIM_LIB ${CMAKE_CURRENT_LIST_DIR}/../tools/gnm-engine/glshim/libglshim.a)
set(OPENGNM_LIB /tmp/kodi-ps4/opengnm/libopengnm.a)
# Only the Kodi executable needs our renderer.  External dependency builds
# (FreeType, FFmpeg, ...) also load this toolchain for compiler checks; if
# the archives are not present yet, do not make their link tests depend on
# them (a missing libopengnm.a breaks FreeType's try_compile).
if(EXISTS "${GLSHIM_LIB}" AND EXISTS "${OPENGNM_LIB}")
  link_libraries(${GLSHIM_LIB} ${OPENGNM_LIB} SceGnmDriver SceVideoOut ScePad
                 SceUserService SceSysmodule SceSystemService SceShellCoreUtil SceRtc SceNet)
else()
  link_libraries(ScePad SceUserService SceSysmodule SceSystemService SceShellCoreUtil SceRtc SceNet)
endif()

# PS4 libc provides iconv in libc.a; skip CMake's link test which
# fails under -nostdlib + link.x.
set(Iconv_IS_BUILT_IN TRUE)
# Staging host-root pins (Unit A): PS4SDK lacks these headers, so point
# only these four at a staging dir with just the needed headers.
# Never point at /usr/include directly: -I/usr/include shadows PS4SDK's
# wchar.h with glibc's, which PS4 clang cannot parse.
set(OPENSSL_INCLUDE_DIR /tmp/kodi-deps-ps4/include)
set(OPENSSL_CRYPTO_LIBRARY /tmp/kodi-deps-ps4/lib/libcrypto.a)
set(OPENSSL_SSL_LIBRARY /tmp/kodi-deps-ps4/lib/libssl.a)
set(Fontconfig_INCLUDE_DIR /tmp/kodi-ps4-host-includes)
set(Fontconfig_LIBRARY /usr/lib/libfontconfig.so)
set(TINYXML_INCLUDE_DIR /tmp/kodi-deps-ps4/include)
set(TINYXML_LIBRARY_RELEASE /tmp/kodi-deps-ps4/lib/libtinyxml.a)
# zlib is cross built for the target by scripts/setup-m1-deps.sh.
set(ZLIB_ROOT /tmp/kodi-deps-ps4)
set(ZLIB_INCLUDE_DIR /tmp/kodi-deps-ps4/include)
set(ZLIB_LIBRARY /tmp/kodi-deps-ps4/lib/libz.a)
# EGL and GLES2 are provided by libglshim.a (tools/gnm-engine/glshim).
set(EGL_INCLUDE_DIR "${PS4SDK}/include" CACHE PATH "EGL headers")
set(OPENGLES_INCLUDE_DIR "${PS4SDK}/include" CACHE PATH "GLES2 headers")
add_definitions(-D__ORBIS__ -DORBIS -DHAS_GLES=2 -DTARGET_POSIX -DTIXML_USE_STL)
# PS4 macro intentionally omitted: -DPS4 expands namespace PS4 to 1.
set(PS4 ON)
set(ORBIS ON)