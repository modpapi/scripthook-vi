# Top-level build for SHVI_PLATFORM=sim (Linux/macOS desktop) and SHVI_PLATFORM=ps5 (console).
# The Windows game core (src/core) is win64 only; everything here is the portable engine, the samples
# built as mods for this platform, and on ps5 the ELF loader payload.
enable_testing()

shvi_add_mod(hello_engine SOURCES samples/hello_engine/hello_engine.c)
shvi_add_mod(menu_demo SOURCES samples/menu_demo/menu_demo.cpp)
shvi_add_mod(trainer SOURCES samples/trainer/trainer_main.cpp)

if(SHVI_PLATFORM STREQUAL "ps5")
  shvi_add_ps5_loader()
  return()
endif()

# sim: the same engine checks as the Windows tree runs (cmake/engine.cmake), minus the Windows core.
install(TARGETS shvi_sim RUNTIME DESTINATION tools)
add_test(NAME sim_hello_engine_hash COMMAND shvi_sim $<TARGET_FILE:hello_engine> --frames 400
         --expect "SDK = 0x" --expect "hello_engine: running on GTA VI (simulated)")
add_test(NAME sim_trainer_menu COMMAND shvi_sim $<TARGET_FILE:trainer> --frames 240 --bind-all --draw
         --key 30:F4 --key 60:Down --expect "trainer: started")
add_test(NAME sim_guard_latch COMMAND shvi_sim $<TARGET_FILE:menu_demo> --frames 120 --block-at 60
         --expect "online guard latched")

add_executable(shvi_engine_tests tests/engine/engine_tests.cpp ${SHVI_ROOT}/platforms/ps5/runtime/pad_map.c)
_shvi_target_defaults(shvi_engine_tests)
target_include_directories(shvi_engine_tests PRIVATE ${SHVI_ROOT}/platforms/ps5/runtime)
target_link_libraries(shvi_engine_tests PRIVATE shvi_engine)
add_test(NAME engine_unit COMMAND shvi_engine_tests)

foreach(t hello trainer)
  if(t STREQUAL "hello")
    set(src samples/hello_engine/hello_engine.c)
  else()
    set(src samples/trainer/trainer_main.cpp)
  endif()
  add_executable(ps5_dryrun_${t} ${src} ${SHVI_ENGINE_SOURCES} ${SHVI_PS5_RUNTIME_SOURCES})
  _shvi_target_defaults(ps5_dryrun_${t})
  target_include_directories(ps5_dryrun_${t} PRIVATE ${SHVI_ROOT}/platforms/ps5/runtime)
  target_link_libraries(ps5_dryrun_${t} PRIVATE Threads::Threads)
endforeach()
add_test(NAME ps5_dryrun_hello COMMAND ps5_dryrun_hello 2)
add_test(NAME ps5_dryrun_trainer COMMAND ps5_dryrun_trainer 3 menu)
set_tests_properties(ps5_dryrun_hello PROPERTIES PASS_REGULAR_EXPRESSION "mod: shvi_plugin_load -> 0.*dry run done")
set_tests_properties(ps5_dryrun_trainer PROPERTIES PASS_REGULAR_EXPRESSION "trainer: key open -> menu open.*dry run done, 180 frames, 1 script")

# Host-side tests of the PS5 payload's pure logic (process selection).
add_executable(ps5_test_proc platforms/ps5/tests/test_proc.c platforms/ps5/payload/proc_select.c)
add_test(NAME ps5_proc_select COMMAND ps5_test_proc)
