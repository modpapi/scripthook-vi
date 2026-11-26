# Portable engine pieces built inside the Windows (win64) tree: the simulator and desktop builds of the
# PS5 runtime, so both are compiled and tested on every PC build too.
shvi_engine_library()
shvi_sim_runner()
install(TARGETS shvi_sim RUNTIME DESTINATION tools)

# hello_engine in the simulator: the sim answers STRING_HASH exactly, so the mod's own check must agree.
add_test(NAME sim_hello_engine_hash COMMAND shvi_sim $<TARGET_FILE:hello_engine> --frames 400 --frame-ms 16
         --expect "SDK = 0x" --expect "hello_engine: running on GTA VI (simulated)")
# The trainer's menu, driven by keys, drawn as text by the sim (UI_DRAW_TEXT).
add_test(NAME sim_trainer_menu COMMAND shvi_sim $<TARGET_FILE:trainer> --frames 240 --bind-all --draw
         --key 30:F4 --key 60:Down --expect "trainer: started")
# Online guard: latching stops every script; the run must still end cleanly.
add_test(NAME sim_guard_latch COMMAND shvi_sim $<TARGET_FILE:menu_demo> --frames 120 --block-at 60
         --expect "online guard latched")

# Engine unit tests (scheduler, host ABI contract, PS5 pad mapping).
add_executable(shvi_engine_tests tests/engine/engine_tests.cpp ${SHVI_ROOT}/platforms/ps5/runtime/pad_map.c)
_shvi_target_defaults(shvi_engine_tests)
target_include_directories(shvi_engine_tests PRIVATE ${SHVI_ROOT}/platforms/ps5/runtime)
target_link_libraries(shvi_engine_tests PRIVATE shvi_engine)
add_test(NAME engine_unit COMMAND shvi_engine_tests)

# The PS5 runtime + a mod as one image, built for the desktop: the same "dry run" a console runs.
function(shvi_ps5_dryrun_test name mod_source)
  add_executable(${name} ${mod_source} ${SHVI_ENGINE_SOURCES} ${SHVI_PS5_RUNTIME_SOURCES})
  _shvi_target_defaults(${name})
  target_include_directories(${name} PRIVATE ${SHVI_ROOT}/platforms/ps5/runtime)
  add_test(NAME ${name} COMMAND ${name} ${ARGN})
endfunction()
shvi_ps5_dryrun_test(ps5_dryrun_hello samples/hello_engine/hello_engine.c 2)
shvi_ps5_dryrun_test(ps5_dryrun_trainer samples/trainer/trainer_main.cpp 3 menu)
set_tests_properties(ps5_dryrun_hello PROPERTIES PASS_REGULAR_EXPRESSION "mod: shvi_plugin_load -> 0.*dry run done")
set_tests_properties(ps5_dryrun_trainer PROPERTIES PASS_REGULAR_EXPRESSION "trainer: key open -> menu open.*dry run done, 180 frames, 1 script")
