# SPDX-License-Identifier: Apache-2.0 OR MIT
cmake_minimum_required(VERSION 3.22)
project(godot_cineform CXX C)

# The arm64 slice runs on nothing older than Big Sur; without this CMake stamps the build host's SDK.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Both dependencies are vendored at a pinned commit rather than tracked on a branch. The SDK
# is quiet enough that a moving branch is a silent dependency change nobody would notice.
# Both dependencies come from THIS repository's own manifest, default.xml, which places them
# under thirdparty/. This project is a repo manifest root.
#
#   repo init -u https://github.com/weftspun/godot-cineform -m default.xml
#   repo sync
#
# Not submodules. CLAUDE.md blocklists those: a submodule pins a dependency in a file only git
# reads, invisible to `repo status`, and a bump shows in a diff as a bare hash with no reason.
set(GODOT_CPP_DIR "${CMAKE_SOURCE_DIR}/thirdparty/godot-cpp" CACHE PATH "godot-cpp checkout")
set(CINEFORM_DIR "${CMAKE_SOURCE_DIR}/thirdparty/cineform-sdk" CACHE PATH "cineform-sdk checkout")

foreach(dep "${GODOT_CPP_DIR}" "${CINEFORM_DIR}")
  if(NOT EXISTS "${dep}/CMakeLists.txt")
    message(FATAL_ERROR
      "missing dependency at ${dep}.
"
      "  Both come from this repository's own manifest. From the repository root:
"
      "    repo init -u https://github.com/weftspun/godot-cineform -m default.xml
"
      "    repo sync")
  endif()
endforeach()

# The SDK builds a test tool by default and we do not need it. BUILD_STATIC with
# BUILD_SEPARATED off yields one target, CFHDCodecStatic, holding encoder and decoder. The
# decoder is dead weight for the exporter and is exactly what the playback half will want, so
# one library serves both rungs of the ladder.
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_SEPARATED OFF CACHE BOOL "" FORCE)

add_subdirectory("${GODOT_CPP_DIR}" godot-cpp EXCLUDE_FROM_ALL)
add_subdirectory("${CINEFORM_DIR}" cineform-sdk EXCLUDE_FROM_ALL)

# clang 21 makes these errors by default; on Windows the SDK passes int32_t* where Win32 wants
# LONG* (ReleaseSemaphore in Codec/decoder.c). interactor-av1mkv builds the SDK with the same two.
target_compile_options(CFHDCodecStatic PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types>)

add_library(godot_cineform SHARED
  src/movie_writer_cineform.cpp
  src/register_types.cpp
)
target_include_directories(godot_cineform PRIVATE
  "${CINEFORM_DIR}/Common"
  "${CINEFORM_DIR}/EncoderSDK"
  "${CMAKE_SOURCE_DIR}/src"
)
target_link_libraries(godot_cineform PRIVATE godot-cpp CFHDCodecStatic)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  # Codec/simd_compat.h maps the SSE and MMX intrinsics to NEON for the writer too.
  target_include_directories(godot_cineform PRIVATE "${CINEFORM_DIR}/Codec")
else()
  target_compile_options(godot_cineform PRIVATE -mssse3 -msse4.1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # A shared object cannot take a static libc, so on Linux only the C++ runtime is linked
  # statically. The SDK's encoder calls uuid_generate; libuuid.so.1 ships with util-linux
  # on every desktop distribution, and Debian's libuuid.a is not PIC.
  target_link_options(godot_cineform PRIVATE -static-libstdc++ -static-libgcc)
  target_link_libraries(godot_cineform PRIVATE uuid pthread)
elseif(WIN32)
  target_link_options(godot_cineform PRIVATE -static)
endif()

# Godot loads the library by the exact name in cineform.gdextension. A mismatch here shows up
# as "extension not found" with no hint about which name was expected.
if(WIN32)
  set(PLATFORM "windows")
elseif(APPLE)
  set(PLATFORM "macos")
else()
  set(PLATFORM "linux")
endif()

# x86_64 and arm64. cineform-sdk's Codec/simd_compat.h maps its SSE and MMX intrinsics to
# NEON through sse2neon and mmx2neon, so a third architecture means a mapping there.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  set(ARCH "arm64")
else()
  message(FATAL_ERROR
    "godot-cineform builds for x86_64 and arm64, not ${CMAKE_SYSTEM_PROCESSOR}: "
    "cineform-sdk's Codec/simd_compat.h maps SSE to NEON and nothing else.")
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" BUILD_LOWER)
if(BUILD_LOWER STREQUAL "debug")
  set(VARIANT "template_debug")
else()
  set(VARIANT "template_release")
endif()
# A double-precision engine loads only the entries tagged `double`; a single-precision
# library in it crashes at load, because the builtin sizes differ.
set(PRECISION_TAG "")
if(GODOT_PRECISION STREQUAL "double")
  set(PRECISION_TAG ".double")
endif()
set_target_properties(godot_cineform PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "godot_cineform.${PLATFORM}.${VARIANT}${PRECISION_TAG}.${ARCH}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/demo/addons/cineform/bin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/demo/addons/cineform/bin"
)
