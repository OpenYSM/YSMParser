# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required(VERSION 3.12...3.31)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" YSM_PARSER_VERSION_RAW)
string(STRIP "${YSM_PARSER_VERSION_RAW}" YSM_PARSER_VERSION)
project("YSMParser" VERSION ${YSM_PARSER_VERSION})

option(YSM_TARGET_WASM "Build a WebAssembly/Node.js target with Emscripten" OFF)
option(YSM_WASM_SIMD "Enable WebAssembly SIMD code generation" OFF)
set(YSM_WASM_ENV "node" CACHE STRING "Emscripten runtime environment (node or web)")
set_property(CACHE YSM_WASM_ENV PROPERTY STRINGS node web)

if(YSM_TARGET_WASM AND NOT YSM_WASM_ENV MATCHES "^(node|web)$")
  message(FATAL_ERROR "YSM_WASM_ENV must be either 'node' or 'web'")
endif()

option(YSM_TARGET_JNI "Build a JNI shared library for Java integration" OFF)

if(YSM_TARGET_JNI)
  if(ANDROID)
    # Android NDK ships jni.h in its sysroot; CMake's FindJNI is unreliable here.
    message(STATUS "Android target: using NDK sysroot JNI headers")
  else()
    find_package(JNI REQUIRED)
    message(STATUS "JNI include dirs: ${JNI_INCLUDE_DIRS}")
  endif()
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(YSM_ZLIB_ZCONF_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib/zconf.h")
set(YSM_ZLIB_ZCONF_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib/zconf.h.in")
if(NOT EXISTS "${YSM_ZLIB_ZCONF_HEADER}" AND EXISTS "${YSM_ZLIB_ZCONF_TEMPLATE}")
  message(STATUS "Bootstrapping external/zlib/zconf.h from zconf.h.in")
  configure_file("${YSM_ZLIB_ZCONF_TEMPLATE}" "${YSM_ZLIB_ZCONF_HEADER}" COPYONLY)
endif()

# Include sub-projects.
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Disable zstd shared library build" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "Enable zstd static library build" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Disable zstd CLI programs" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Disable zstd tests" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "Disable zstd contrib targets" FORCE)
if(YSM_TARGET_WASM)
  set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "Disable zstd threading for wasm builds" FORCE)
endif()
add_subdirectory("external/zstd/build/cmake" "external/zstd/build_out")
add_subdirectory("external/cityhash")
add_subdirectory("external/xchacha20")
add_subdirectory("external/AES")
add_subdirectory("external/md5")
add_subdirectory("external/cpp-base64")

set(ZLIB_BUILD_STATIC ON CACHE BOOL "Build zlib static library" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "Disable zlib shared library build" FORCE)

# zlib writes ZLIB_CONF_WRITTEN to cache after creating zconf.h.cmakein.
# If the build tree is partially cleaned, the cache variable stays TRUE but
# the .cmakein file is gone, causing a configure error. Force regeneration.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/external/zlib/zconf.h.cmakein")
  unset(ZLIB_CONF_WRITTEN CACHE)
endif()

add_subdirectory("external/zlib")

add_library(fpng STATIC "external/fpng/src/fpng.cpp")
target_include_directories(fpng PUBLIC "external/fpng/src")
target_compile_definitions(fpng PRIVATE FPNG_NO_SSE=1)

if(YSM_TARGET_WASM)
  set(CMAKE_EXECUTABLE_SUFFIX ".js")
endif()


add_subdirectory ("YSMParser")
