# CMake toolchain for PS5 mods: cmake -DCMAKE_TOOLCHAIN_FILE=<sdk>/sdk/cmake/toolchains/ps5.cmake
#
# Needs the open-source ps5-payload-sdk (https://github.com/ps5-payload-dev/sdk, v0.50 tested) and
# PS5_PAYLOAD_SDK pointing at it (default /opt/ps5-payload-sdk). Builds on Linux (or WSL).
# Only open-source homebrew tooling is used: no Sony SDK material.
set(SHVI_PLATFORM ps5 CACHE STRING "ScriptHook VI target platform" FORCE)

if(DEFINED ENV{PS5_PAYLOAD_SDK})
  set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
elseif(NOT PS5_PAYLOAD_SDK)
  set(PS5_PAYLOAD_SDK /opt/ps5-payload-sdk)
endif()
if(NOT EXISTS "${PS5_PAYLOAD_SDK}/bin")
  message(FATAL_ERROR "ps5-payload-sdk not found at '${PS5_PAYLOAD_SDK}'. Install it and set PS5_PAYLOAD_SDK "
                      "(the shvi CLI does both: shvi doctor --install-ps5-sdk).")
endif()
# Try-compile runs in a fresh scope: hand it the SDK path.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS5_PAYLOAD_SDK)

if(EXISTS "${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
  # The SDK's own toolchain (compilers, sysroot, link flags) when it ships one.
  include("${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
else()
  set(CMAKE_SYSTEM_NAME FreeBSD)
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
  set(CMAKE_C_COMPILER "${PS5_PAYLOAD_SDK}/bin/prospero-clang")
  set(CMAKE_CXX_COMPILER "${PS5_PAYLOAD_SDK}/bin/prospero-clang++")
  set(CMAKE_FIND_ROOT_PATH "${PS5_PAYLOAD_SDK}/target")
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
