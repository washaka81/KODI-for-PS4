add_library(kodi_ps4_platform STATIC
  PlatformPS4.cpp
  PS4AudioSink.cpp
  PS4RendererGLES2.cpp
  PS4Input.cpp
  CAESinkPS4.cpp
)

target_include_directories(kodi_ps4_platform PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(KODI_PS4_ENABLE_GLES)
  target_compile_definitions(kodi_ps4_platform PUBLIC KODI_PS4_USE_GLES=1)
endif()
if(KODI_PS4_ENABLE_VDEC)
  target_compile_definitions(kodi_ps4_platform PUBLIC KODI_PS4_USE_VDEC=1)
endif()
if(KODI_PS4_ENABLE_PYTHON)
  target_compile_definitions(kodi_ps4_platform PUBLIC KODI_PS4_ENABLE_PYTHON=1)
endif()

target_compile_options(kodi_ps4_platform PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(kodi-ps4 main_stub.cpp)
target_link_libraries(kodi-ps4 PRIVATE kodi_ps4_platform)
set_target_properties(kodi-ps4 PROPERTIES
  OUTPUT_NAME "kodi-ps4"
  SUFFIX ".elf"
)