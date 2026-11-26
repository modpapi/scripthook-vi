# ScriptHook VI SDK - build a mod for any platform from one source tree.
#
#   cmake_minimum_required(VERSION 3.24)
#   project(my_mod LANGUAGES C CXX)
#   include(<sdk>/sdk/cmake/ShviMod.cmake)       # <sdk> = this repo, or an unpacked shvi-sdk release
#   shvi_add_mod(my_mod SOURCES src/main.cpp)
#
# The platform comes from SHVI_PLATFORM (cache variable):
#   win64  MSVC x64 on Windows (default there)  -> my_mod.asi       install: game/shvi/plugins/
#   ps5    sdk/cmake/toolchains/ps5.cmake        -> my_mod.elf       install: ps5/shvi/mods/
#          (one self-contained image: the mod + the PS5 runtime + the portable engine)
#   sim    any other desktop compiler            -> my_mod.so        install: sim/shvi/plugins/
#
# On win64 and sim the SDK also builds shvi_sim (the offline simulator) and, when the project calls
# enable_testing(), adds a test "<mod>_sim" that loads the mod and runs 300 simulated frames.
# On ps5 it can build the ELF loader payload with shvi_add_ps5_loader().
include_guard(GLOBAL)

get_filename_component(SHVI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SHVI_SDK_INCLUDE "${SHVI_ROOT}/sdk/include")

if(NOT SHVI_PLATFORM)
  if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MSVC)
    set(_shvi_default win64)
  else()
    set(_shvi_default sim)
  endif()
  set(SHVI_PLATFORM ${_shvi_default} CACHE STRING "ScriptHook VI target platform: win64, ps5 or sim")
endif()
set_property(CACHE SHVI_PLATFORM PROPERTY STRINGS win64 ps5 sim)
if(NOT SHVI_PLATFORM MATCHES "^(win64|ps5|sim)$")
  message(FATAL_ERROR "SHVI_PLATFORM must be win64, ps5 or sim (got '${SHVI_PLATFORM}')")
endif()
if(SHVI_PLATFORM STREQUAL "win64" AND NOT MSVC)
  message(FATAL_ERROR "SHVI_PLATFORM=win64 needs MSVC (Visual Studio 2022 or newer, x64)")
endif()
string(TOUPPER ${SHVI_PLATFORM} _SHVI_PLATFORM_UP)

option(SHVI_BUILD_SIM "Build shvi_sim and a <mod>_sim test for every mod (win64, sim)" ON)

set(SHVI_ENGINE_SOURCES
  ${SHVI_ROOT}/src/engine/host.cpp
  ${SHVI_ROOT}/src/engine/scheduler.cpp)
set(SHVI_PS5_RUNTIME_SOURCES
  ${SHVI_ROOT}/platforms/ps5/runtime/runtime.cpp
  ${SHVI_ROOT}/platforms/ps5/runtime/runtime_main.cpp
  ${SHVI_ROOT}/platforms/ps5/runtime/pad_map.c)

# Common compile settings for anything the SDK builds.
function(_shvi_target_defaults tgt)
  set_target_properties(${tgt} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON C_STANDARD 11)
  target_include_directories(${tgt} PRIVATE ${SHVI_SDK_INCLUDE} ${SHVI_ROOT}/src)
  target_compile_definitions(${tgt} PRIVATE SHVI_PLATFORM_${_SHVI_PLATFORM_UP}=1)
  if(MSVC)
    set_target_properties(${tgt} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(${tgt} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${tgt} PRIVATE /utf-8 /bigobj $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
  else()
    target_compile_options(${tgt} PRIVATE -fPIC)
  endif()
endfunction()

# The portable engine as a static library (sim runner; desktop builds of the PS5 runtime).
function(shvi_engine_library)
  if(TARGET shvi_engine)
    return()
  endif()
  add_library(shvi_engine STATIC ${SHVI_ENGINE_SOURCES} ${SHVI_ROOT}/src/engine/plugin_loader.cpp)
  _shvi_target_defaults(shvi_engine)
  target_include_directories(shvi_engine PUBLIC ${SHVI_SDK_INCLUDE} ${SHVI_ROOT}/src)
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(shvi_engine PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  endif()
endfunction()

# shvi_sim: the offline simulator (tools/sim/main.cpp).
function(shvi_sim_runner)
  if(TARGET shvi_sim OR SHVI_PLATFORM STREQUAL "ps5")
    return()
  endif()
  shvi_engine_library()
  add_executable(shvi_sim ${SHVI_ROOT}/tools/sim/main.cpp)
  _shvi_target_defaults(shvi_sim)
  target_link_libraries(shvi_sim PRIVATE shvi_engine)
  if(NOT WIN32)
    set_target_properties(shvi_sim PROPERTIES ENABLE_EXPORTS ON)
  endif()
endfunction()

# shvi_add_mod(<name> SOURCES <files...> [INCLUDES <dirs...>] [DEFINES <defs...>] [LIBS <libs...>]
#              [SIM_ARGS <shvi_sim args...>] [NO_INSTALL])
function(shvi_add_mod name)
  cmake_parse_arguments(M "NO_INSTALL" "" "SOURCES;INCLUDES;DEFINES;LIBS;SIM_ARGS" ${ARGN})
  if(NOT M_SOURCES)
    message(FATAL_ERROR "shvi_add_mod(${name}): SOURCES is required")
  endif()

  if(SHVI_PLATFORM STREQUAL "ps5")
    # One image: mod + runtime + engine, in the payload ELF format the console's ELF loader runs.
    add_executable(${name} ${M_SOURCES} ${SHVI_ENGINE_SOURCES} ${SHVI_PS5_RUNTIME_SOURCES})
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")
    target_include_directories(${name} PRIVATE ${SHVI_ROOT}/platforms/ps5/runtime)
    set(_dest ps5/shvi/mods)
  else()
    if(SHVI_PLATFORM STREQUAL "win64")
      add_library(${name} SHARED ${M_SOURCES})
      set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".asi")
      set(_dest game/shvi/plugins)
    else()
      add_library(${name} MODULE ${M_SOURCES})
      set_target_properties(${name} PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
      if(WIN32)
        set_target_properties(${name} PROPERTIES SUFFIX ".dll")
      else()
        set_target_properties(${name} PROPERTIES SUFFIX ".so")
      endif()
      set(_dest sim/shvi/plugins)
    endif()
    # Mods built to <build>/shvi/plugins/, the layout shvi_sim and the game expect.
    set_target_properties(${name} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/shvi/plugins
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/shvi/plugins)
    foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
      string(TOUPPER ${cfg} CFG)
      set_target_properties(${name} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/shvi/plugins
        RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/shvi/plugins)
    endforeach()
  endif()

  _shvi_target_defaults(${name})
  if(M_INCLUDES)
    target_include_directories(${name} PRIVATE ${M_INCLUDES})
  endif()
  if(M_DEFINES)
    target_compile_definitions(${name} PRIVATE ${M_DEFINES})
  endif()
  if(M_LIBS)
    target_link_libraries(${name} PRIVATE ${M_LIBS})
  endif()
  if(NOT M_NO_INSTALL)
    install(TARGETS ${name} RUNTIME DESTINATION ${_dest} LIBRARY DESTINATION ${_dest})
  endif()

  if(SHVI_BUILD_SIM AND NOT SHVI_PLATFORM STREQUAL "ps5")
    shvi_sim_runner()
    if(NOT M_SIM_ARGS)
      set(M_SIM_ARGS --frames 300 --key 30:F4 --key 40:Down --key 50:Return --key 60:Back --key 70:F4)
    endif()
    add_test(NAME ${name}_sim COMMAND shvi_sim $<TARGET_FILE:${name}> ${M_SIM_ARGS})
  endif()
endfunction()

# shvi_add_ps5_loader(): builds shvi_loader.elf, the payload sent to the console's ELF loader
# (elfldr, port 9021). It finds the game process and injects (platforms/ps5/payload).
function(shvi_add_ps5_loader)
  if(NOT SHVI_PLATFORM STREQUAL "ps5" OR TARGET shvi_loader)
    return()
  endif()
  set(P ${SHVI_ROOT}/platforms/ps5/payload)
  add_executable(shvi_loader ${P}/main.c ${P}/proc.c ${P}/proc_select.c ${P}/inject.c)
  set_target_properties(shvi_loader PROPERTIES SUFFIX ".elf" C_STANDARD 11)
  target_compile_options(shvi_loader PRIVATE -Wall -Wextra -O2)
  install(TARGETS shvi_loader RUNTIME DESTINATION ps5)
endfunction()
