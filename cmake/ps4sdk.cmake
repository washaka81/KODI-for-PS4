## ps4sdk.cmake - OpenOrbis PS4 cross-compile file
## Usage:
##   export OO_PS4_TOOLCHAIN=/opt/ps4/sdk   # or /opt/ps4/oosdk
##   export PS4SDK=$OO_PS4_TOOLCHAIN
##   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/ps4sdk.cmake

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

if(NOT EXISTS "${PS4SDK}")
  message(FATAL_ERROR "PS4SDK/OO_PS4_TOOLCHAIN not found at ${PS4SDK}")
endif()

set(PS4TARGET "${PS4SDK}/target")
set(PS4HOST "${PS4SDK}/host_tools")
set(CMAKE_FIND_ROOT_PATH "${PS4TARGET}")

if(EXISTS "${PS4HOST}/bin/orbis-clang")
  set(toolPrefix "orbis-")
  set(toolSuffix "")
  set(CMAKE_C_COMPILER "${PS4HOST}/bin/${toolPrefix}clang${toolSuffix}")
  set(CMAKE_CXX_COMPILER "${PS4HOST}/bin/${toolPrefix}clang++${toolSuffix}")
else()
  set(CMAKE_C_COMPILER clang)
  set(CMAKE_CXX_COMPILER clang++)
  set(CMAKE_C_COMPILER_TARGET x86_64-scei-ps4)
  set(CMAKE_CXX_COMPILER_TARGET x86_64-scei-ps4)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)

include_directories(
  "${PS4TARGET}/include"
  "${PS4TARGET}/include_common"
  "${PS4SDK}/include"
)

set(CMAKE_EXE_LINKER_FLAGS
  "${CMAKE_EXE_LINKER_FLAGS} -s -Wl,--addressing=non-aslr,--strip-unused-data -L${PS4TARGET}/lib -L${PS4SDK}/lib"
)

add_definitions(-DPS4 -D__ORBIS__ -DORBIS)

set(PS4_STUB_LIBS
  SceSystemService_stub_weak
  SceUserService_stub_weak
  ScePosix_stub_weak
  ScePad_stub_weak
  SceAudioOut_stub_weak
  SceSysmodule_stub_weak
  ScePigletv2VSH_stub_weak
  SceGnmDriver_stub_weak
  SceVideoOut_stub_weak
  SceVideodec2_stub_weak
  libc_stub_weak.a
)

if(CMAKE_EXE_LINKER_FLAGS)
  link_libraries(${PS4_STUB_LIBS})
endif()

set(PS4 ON)
set(ORBIS ON)
set(binSuffix ".elf")
