# nativedb ctest driver: cmake -DMODE=<mode> -DTOOL=<shvi_nativedb.exe> -DWORK=<dir> [...] -P check.cmake
#   golden     -DEXE= -DGOLDEN=   build into WORK/run1, census.tsv and manifest.json equal the golden files
#   repro      -DEXE=             build into run2 twice (over itself) and run3 fresh; folders identical
#   bad_input  -DEXE=             build must exit 1 and leave no census.tsv anywhere under WORK/bad
#   stale_out  -DEXE= -DGOLDEN=   build over a folder holding junk census.tsv + unrelated notes.txt
#   real       -DEXE= -DEXPECT=   build the real exe; census.tsv equals EXPECT (opt-in Xbox test)
include("${CMAKE_CURRENT_LIST_DIR}/compare_dirs.cmake")

set(KEY 1592590337)  # 0x5EED0001, the synthetic exe's TimeDateStamp

function(run_build out expect_rc)
  execute_process(COMMAND "${TOOL}" build --exe "${EXE}" --out "${out}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL expect_rc)
    message(FATAL_ERROR "build --out ${out}: exit ${rc}, expected ${expect_rc}\n${o}${e}")
  endif()
endfunction()

function(same a b)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${a}" "${b}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "files differ:\n  ${a}\n  ${b}")
  endif()
endfunction()

if(MODE STREQUAL "golden")
  file(REMOVE_RECURSE "${WORK}/run1")
  run_build("${WORK}/run1" 0)
  same("${WORK}/run1/${KEY}/census.tsv" "${GOLDEN}/census.tsv")
  same("${WORK}/run1/${KEY}/manifest.json" "${GOLDEN}/manifest.json")

elseif(MODE STREQUAL "repro")
  file(REMOVE_RECURSE "${WORK}/run2" "${WORK}/run3")
  run_build("${WORK}/run2" 0)
  run_build("${WORK}/run2" 0)
  run_build("${WORK}/run3" 0)
  compare_dirs("${WORK}/run2/${KEY}" "${WORK}/run3/${KEY}")

elseif(MODE STREQUAL "bad_input")
  file(REMOVE_RECURSE "${WORK}/bad")
  run_build("${WORK}/bad" 1)
  file(GLOB_RECURSE left "${WORK}/bad/*census.tsv*" "${WORK}/bad/*manifest.json*")
  if(left)
    message(FATAL_ERROR "failed build left files: ${left}")
  endif()

elseif(MODE STREQUAL "stale_out")
  set(d "${WORK}/run4/${KEY}")
  file(REMOVE_RECURSE "${WORK}/run4")
  file(MAKE_DIRECTORY "${d}")
  string(REPEAT "junk junk junk junk junk junk junk junk junk junk junk junk junk junk junk!\n" 13108 junk)
  file(WRITE "${d}/census.tsv" "${junk}")  # ~1 MB, longer than the real census
  file(WRITE "${d}/notes.txt" "keep me\n")
  run_build("${WORK}/run4" 0)
  same("${d}/census.tsv" "${GOLDEN}/census.tsv")
  file(READ "${d}/notes.txt" n)
  if(NOT n STREQUAL "keep me\n")
    message(FATAL_ERROR "notes.txt was changed")
  endif()
  file(READ "${d}/manifest.json" m)
  string(FIND "${m}" "notes.txt" at)
  if(NOT at EQUAL -1)
    message(FATAL_ERROR "manifest.json lists notes.txt")
  endif()
  file(GLOB tmp "${d}/*.tmp")
  if(tmp)
    message(FATAL_ERROR "temp files left: ${tmp}")
  endif()

elseif(MODE STREQUAL "real")
  # EXPECT = the kept nativedb/<build> folder. With -DIDA= -DDUMP= the run also writes
  # census_vs_ida.md (must exit 0: nothing unexplained) and the whole folder must equal EXPECT.
  file(REMOVE_RECURSE "${WORK}/real")
  set(extra "")
  if(IDA)
    set(extra --ida "${IDA}" --dump "${DUMP}")
  endif()
  execute_process(COMMAND "${TOOL}" build --exe "${EXE}" --out "${WORK}/real" ${extra}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "build on the real exe: exit ${rc}\n${o}${e}")
  endif()
  get_filename_component(key "${EXPECT}" NAME)
  same("${WORK}/real/${key}/census.tsv" "${EXPECT}/census.tsv")
  if(IDA)
    compare_dirs("${WORK}/real/${key}" "${EXPECT}")
  endif()

else()
  message(FATAL_ERROR "unknown MODE '${MODE}'")
endif()
