cmake_minimum_required(VERSION 3.20)
project(kodi-ps4 VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(KODI_PS4_ENABLE_GLES "Use Piglet GLES2 as the primary render backend" ON)
option(KODI_PS4_ENABLE_VDEC "Reserve hooks for sceVideodec2* hardware decode" OFF)
option(KODI_PS4_ENABLE_PYTHON "Enable Python/addon support (Phase 4)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_subdirectory(src/platform/ps4)

message(STATUS "kodi-ps4 configured. Target: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  GLES renderer: ${KODI_PS4_ENABLE_GLES}")
message(STATUS "  Hardware VDEC: ${KODI_PS4_ENABLE_VDEC}")
message(STATUS "  Python addons: ${KODI_PS4_ENABLE_PYTHON}")
