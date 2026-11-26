cmake_minimum_required(VERSION 3.24)
project(scripthook_vi VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ScriptHook VI is x64 only")
endif()

# ---------------------------------------------------------------- platform
# One tree, three platforms (sdk/include/shvi/shvi.h, "Platforms"):
#   win64  MSVC on Windows: the game core, proxy loaders, tools, samples as .asi, all tests
#   ps5    -DCMAKE_TOOLCHAIN_FILE=sdk/cmake/toolchains/ps5.cmake: the ELF loader payload and the
#          samples as self-contained PS5 mod images (<mod>.elf)
#   sim    any other desktop compiler (Linux/macOS): the engine, shvi_sim and the samples as .so
include(sdk/cmake/ShviMod.cmake)
message(STATUS "ScriptHook VI platform: ${SHVI_PLATFORM}")
if(NOT SHVI_PLATFORM STREQUAL "win64")
  include(cmake/portable.cmake)
  return()
endif()
enable_language(ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
# Static CRT: nothing extra to install into the game folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)

# ---------------------------------------------------------------- MinHook (pinned)
include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        c3fcafdc10146beb5919319d0683e44e3c30d537  # v1.3.4
  SOURCE_SUBDIR  _no_cmake_)                                 # we build it ourselves
FetchContent_MakeAvailable(minhook)
add_library(minhook STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)
target_compile_options(minhook PRIVATE /W0)

# ---------------------------------------------------------------- generic core (static lib)
add_library(shvi_core_lib STATIC
  src/core/log.cpp
  src/core/pe.cpp
  src/core/pattern.cpp
  src/core/crossmap.cpp
  src/core/invoker.cpp
  src/core/fiber_scheduler.cpp
  src/core/online_guard.cpp
  src/core/probes.cpp
  src/core/denylist.cpp
  src/core/native_table.cpp
  src/core/trampoline.cpp
  src/core/overlay_drawlist.cpp)
target_include_directories(shvi_core_lib PUBLIC src sdk/include)

# ---------------------------------------------------------------- Dear ImGui (pinned) + D3D12 overlay (T7.1)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        c1aaac65e896a45f81f47a4d68f9959e0f86c859  # v1.93.0
  SOURCE_SUBDIR  _no_cmake_)                                 # we build it ourselves
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(imgui PRIVATE /W0)

add_library(shvi_overlay STATIC src/core/overlay_d3d12.cpp src/core/overlay_vtables.cpp)
target_link_libraries(shvi_overlay PUBLIC shvi_core_lib minhook imgui d3d12 dxgi d3dcompiler)

# ---------------------------------------------------------------- game profiles (static lib)
add_library(shvi_profiles STATIC
  src/games/gta6/gta6_profile.cpp)
target_link_libraries(shvi_profiles PUBLIC shvi_core_lib minhook)

# ---------------------------------------------------------------- core DLL
add_library(shvi_core SHARED src/core/core_main.cpp)
target_link_libraries(shvi_core PRIVATE shvi_profiles shvi_overlay)

# ---------------------------------------------------------------- proxy loaders
set(SHVI_PROXIES dinput8 version CACHE STRING "Proxy DLLs to build (see tools/pick_proxy)")
foreach(p IN LISTS SHVI_PROXIES)
  add_library(shvi_proxy_${p} SHARED
    src/loader/proxy_main.cpp
    src/loader/proxy_stubs.asm
    src/loader/proxies/${p}.cpp
    src/loader/proxies/${p}.def)
  set_target_properties(shvi_proxy_${p} PROPERTIES OUTPUT_NAME ${p}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/proxies/${p})
  install(TARGETS shvi_proxy_${p} RUNTIME DESTINATION proxies/${p})
endforeach()

# ---------------------------------------------------------------- tools, samples, tests
add_executable(shvi_pick_proxy tools/pick_proxy/main.cpp)
target_link_libraries(shvi_pick_proxy PRIVATE shvi_core_lib)

# Offline signature check: shvi_scan <exe> maps the file and resolves the matching profile's targets.
add_executable(shvi_scan tools/scan/main.cpp)
target_link_libraries(shvi_scan PRIVATE shvi_profiles)

# Samples are built with the SDK's shvi_add_mod(), exactly like a mod made with the shvi CLI.
shvi_add_mod(hello_engine SOURCES samples/hello_engine/hello_engine.c NO_INSTALL)
shvi_add_mod(menu_demo SOURCES samples/menu_demo/menu_demo.cpp NO_INSTALL)
# The trainer: mod-menu logic on the generated NativeDB wrappers (sdk/include/shvi/natives_full.hpp).
shvi_add_mod(trainer SOURCES samples/trainer/trainer_main.cpp NO_INSTALL)

# ---------------------------------------------------------------- portable engine (sim, PS5 runtime)
include(cmake/engine.cmake)

enable_testing()
add_executable(shvi_trainer_tests tests/trainer_tests.cpp)
target_include_directories(shvi_trainer_tests PRIVATE sdk/include)
target_compile_options(shvi_trainer_tests PRIVATE /bigobj)
add_test(NAME trainer_logic COMMAND shvi_trainer_tests)
add_executable(shvi_trainer_summary tests/trainer_summary.cpp)
target_include_directories(shvi_trainer_summary PRIVATE sdk/include)
target_compile_options(shvi_trainer_summary PRIVATE /bigobj)
add_executable(shvi_tests tests/tests.cpp)
target_link_libraries(shvi_tests PRIVATE shvi_profiles)
add_test(NAME shvi_tests COMMAND shvi_tests)
target_compile_definitions(shvi_tests PRIVATE SHVI_SOURCE_DIR_W=L"${CMAKE_CURRENT_SOURCE_DIR}")

# T2.10: sdk/include/shvi/natives_vi.hpp must match the native table it is generated from.
find_program(SHVI_NODE node)
if(SHVI_NODE)
  add_test(NAME natives_header_current COMMAND ${SHVI_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/docgen/gen_natives.js
           crossmap/gta6_xbox_1.0.2.1.json sdk/include/shvi/natives_vi.hpp --check
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---------------------------------------------------------------- install = game folder layout
# cmake --install build --config Release --prefix dist
#   dist/game/dinput8.dll, dist/game/shvi/shvi_core.dll, dist/game/shvi/plugins/hello_engine.asi
install(TARGETS shvi_proxy_dinput8 RUNTIME DESTINATION game)
install(TARGETS shvi_core RUNTIME DESTINATION game/shvi)
install(TARGETS hello_engine menu_demo trainer RUNTIME DESTINATION game/shvi/plugins)
install(TARGETS shvi_pick_proxy shvi_scan RUNTIME DESTINATION tools)
install(FILES sdk/include/shvi/shvi.h sdk/include/shvi/shvi.hpp sdk/include/shvi/ui.hpp
              sdk/include/shvi/vi_ui.hpp sdk/include/shvi/natives_vi.hpp DESTINATION sdk/include/shvi)
# Native table (T2.3): the profile loads <game>\shvi\natives\<PE timestamp>.json.
install(FILES crossmap/gta6_xbox_1.0.2.1.json DESTINATION game/shvi/natives RENAME 1794606036.json)

# ---------------------------------------------------------------- NativeDB pipeline (Phase 4b)
# Zydis + Zycore (pinned). The commits go into every manifest.json the tool writes.
set(SHVI_ZYCORE_COMMIT 0b2432ced0884fd152b471d97ecf0258ff4d859f)  # zycore-c v1.5.2 (Zydis v4.1.1's submodule)
set(SHVI_ZYDIS_COMMIT  a2278f1d254e492f6a6b39f6cb5d1f5d515659dc)  # zydis v4.1.1
FetchContent_Declare(zycore
  GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
  GIT_TAG        ${SHVI_ZYCORE_COMMIT}  # v1.5.2
  SOURCE_SUBDIR  _no_cmake_)            # Zydis adds it itself via ZYAN_ZYCORE_PATH
FetchContent_MakeAvailable(zycore)
set(ZYAN_ZYCORE_PATH ${zycore_SOURCE_DIR} CACHE PATH "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis
  GIT_REPOSITORY https://github.com/zyantific/zydis.git
  GIT_TAG        ${SHVI_ZYDIS_COMMIT}   # v4.1.1
  GIT_SUBMODULES "")                    # Zycore comes from the pin above
FetchContent_MakeAvailable(zydis)

add_library(shvi_nativedb_lib STATIC
  tools/nativedb/image.cpp
  tools/nativedb/disasm.cpp
  tools/nativedb/pdata.cpp
  tools/nativedb/census.cpp
  tools/nativedb/manifest.cpp
  tools/nativedb/compare.cpp)
target_include_directories(shvi_nativedb_lib PUBLIC tools/nativedb src)
target_link_libraries(shvi_nativedb_lib PUBLIC shvi_core_lib Zydis)
target_compile_definitions(shvi_nativedb_lib PUBLIC
  SHVI_ZYDIS_COMMIT="${SHVI_ZYDIS_COMMIT}" SHVI_ZYCORE_COMMIT="${SHVI_ZYCORE_COMMIT}"
  SHVI_TOOL_VERSION="${PROJECT_VERSION}")

# Synthetic PE for the golden/repro tests (tests/nativedb/synth_pe.*).
add_library(shvi_nativedb_synth_lib STATIC tests/nativedb/synth_pe.cpp)
target_include_directories(shvi_nativedb_synth_lib PUBLIC tests/nativedb)
add_executable(shvi_nativedb_synth tests/nativedb/synth_main.cpp)
target_link_libraries(shvi_nativedb_synth PRIVATE shvi_nativedb_synth_lib)

add_executable(shvi_nativedb_tests tests/nativedb/tests.cpp)
target_include_directories(shvi_nativedb_tests PRIVATE src tests/nativedb)
target_link_libraries(shvi_nativedb_tests PRIVATE shvi_nativedb_lib shvi_nativedb_synth_lib)
add_test(NAME nativedb_unit COMMAND shvi_nativedb_tests)

# shvi_nativedb census|build|compare-census (docs/superpowers/specs/2026-11-24-nativedb-pipeline-design.md)
add_executable(shvi_nativedb tools/nativedb/main.cpp)
target_link_libraries(shvi_nativedb PRIVATE shvi_nativedb_lib)
install(TARGETS shvi_nativedb RUNTIME DESTINATION tools)

set(SHVI_NDB_WORK ${CMAKE_BINARY_DIR}/nativedb_test)
set(SHVI_NDB_CHECK ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:shvi_nativedb> -DWORK=${SHVI_NDB_WORK}
    -DEXE=${SHVI_NDB_WORK}/synth.exe -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/tests/nativedb/golden)
add_test(NAME nativedb_synth_write COMMAND shvi_nativedb_synth ${SHVI_NDB_WORK}/synth.exe)
set_tests_properties(nativedb_synth_write PROPERTIES FIXTURES_SETUP nativedb_synth)
foreach(mode golden repro stale_out)
  add_test(NAME nativedb_${mode} COMMAND ${SHVI_NDB_CHECK} -DMODE=${mode}
           -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/nativedb/check.cmake)
  set_tests_properties(nativedb_${mode} PROPERTIES FIXTURES_REQUIRED nativedb_synth)
endforeach()
# Review focus: a PE with no handlers (the synth writer exe itself) exits 1 and writes nothing.
add_test(NAME nativedb_bad_input COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:shvi_nativedb> -DWORK=${SHVI_NDB_WORK}
         -DEXE=$<TARGET_FILE:shvi_nativedb_synth> -DMODE=bad_input -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/nativedb/check.cmake)

add_executable(shvi_proxy_test tests/proxy_test.cpp)
add_dependencies(shvi_proxy_test shvi_proxy_version)
add_test(NAME proxy_version_forwards COMMAND shvi_proxy_test $<TARGET_FILE:shvi_proxy_version>)

# Optional: check the GTA VI signature set against a real exe that is NOT in this repo.
#   cmake -B build -DSHVI_GTA6_XBOX_EXE=C:/path/to/game_scarlett_master.exe
set(SHVI_GTA6_XBOX_EXE "" CACHE FILEPATH "GTA VI Xbox exe for the offline signature test (optional)")
if(SHVI_GTA6_XBOX_EXE)
  add_test(NAME scan_gta6_xbox COMMAND shvi_scan ${SHVI_GTA6_XBOX_EXE})
  add_test(NAME crossmap_gta6_xbox COMMAND shvi_scan ${SHVI_GTA6_XBOX_EXE} --crossmap
           ${CMAKE_CURRENT_SOURCE_DIR}/crossmap/gta6_xbox_1.0.2.1.json)
endif()

# Optional: the NativeDB census of the real exe equals the kept nativedb/1794606036/census.tsv; with the
# IDA census (outside the repo) as well, census_vs_ida.md has nothing unexplained and the folder is identical.
#   cmake -B build -DSHVI_GTA6_XBOX_IDA_CENSUS=C:/path/to/analysis/gta6_xbox/natives_census.tsv
set(SHVI_GTA6_XBOX_IDA_CENSUS "" CACHE FILEPATH "IDA natives_census.tsv for the Xbox exe (optional)")
if(SHVI_GTA6_XBOX_EXE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nativedb/1794606036/census.tsv)  # census is kept locally
  set(ndb_ida "")
  if(SHVI_GTA6_XBOX_IDA_CENSUS)
    set(ndb_ida -DIDA=${SHVI_GTA6_XBOX_IDA_CENSUS} -DDUMP=${CMAKE_CURRENT_SOURCE_DIR}/docs/reference/natives_xbox_1.0.2.1.tsv)
  endif()
  add_test(NAME nativedb_gta6_xbox COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:shvi_nativedb> -DWORK=${SHVI_NDB_WORK}
           -DEXE=${SHVI_GTA6_XBOX_EXE} -DEXPECT=${CMAKE_CURRENT_SOURCE_DIR}/nativedb/1794606036 ${ndb_ida}
           -DMODE=real -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/nativedb/check.cmake)
endif()

# ---------------------------------------------------------------- D3D12 overlay test (T7.1, WARP, no game)
add_executable(shvi_overlay_warp tests/overlay/warp_test.cpp)
target_link_libraries(shvi_overlay_warp PRIVATE shvi_overlay)
add_test(NAME overlay_warp_early COMMAND shvi_overlay_warp early)
foreach(mode late fp16 fault drawfault closefault disable stale recreate)
  add_test(NAME overlay_warp_${mode} COMMAND shvi_overlay_warp ${mode})
endforeach()
