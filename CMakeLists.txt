cmake_minimum_required(VERSION 3.16)
project(minijzc CXX)

set(CMAKE_CXX_STANDARD 20)
find_package(Git QUIET)
if(NOT GIT_FOUND)
  set(GIT_EXECUTABLE "git") # hope it is on PATH
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# options
# ---------------------------------------------------------------------------
option(MINIJZ_MULTI_CPP
  "ON: compile the compiler as 8 linked TUs (parallel + incremental build).
       OFF: one translation unit (header-only, the constexpr-friendly form)."
  ON)
set(FUZZ_SEEDS "1:400" CACHE STRING
  "fuzz seed window for ctest, 'start:count' (e.g. 4001:400, 1:2000)")
set(FUZZ_SHARDS 1 CACHE STRING "split FUZZ_SEEDS into this many ctest cases")
option(FUZZ_RANDOM_CONFIGS ON "also fuzz with random pass configurations")
set(MJZ_CX_CHECKS "1;2;3;4;5;6" CACHE STRING "constexpr proof check numbers")

# the mjz_lib clone next to this repo (README: git clone String mjz_lib)
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib/alllib_import.hpp)
  message(FATAL_ERROR
    "mjz_lib not found: 'git clone https://github.com/Mjz86/String mjz_lib'"
    " inside this repo first (see README.md)")
endif()

# ---------------------------------------------------------------------------
# the compiler binary
# ---------------------------------------------------------------------------
if(MINIJZ_MULTI_CPP)
  # 9 real TUs (vs the historical 8 header-only slices that each recompiled
  # the world): the heavy pipeline code is compiled ONCE per binary — tu/main
  # only parses options (light), everything else links through declarations.
  add_executable(minijz
    src/tu/main.cpp src/tu/cli_app.cpp src/tu/debugger.cpp src/tu/demo.cpp
    src/tu/shrink.cpp src/tu/cxtours.cpp src/tu/fuzz.cpp src/tu/prog.cpp
    src/tu/backends.cpp)
  target_compile_definitions(minijz PRIVATE MJZ_MULTI_CPP)
  target_include_directories(minijz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                                           ${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib)
else()
  add_executable(minijz main.cpp)
  target_include_directories(minijz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                                           ${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib)
endif()
# each cx constant expression retains its full evaluation state: give the
# checks generous limits (matches the README's g++ invocation)
target_compile_options(minijz PRIVATE
  $<$<CONFIG:Debug>:-fconstexpr-depth=8192>
  $<$<CONFIG:Debug>:-fconstexpr-ops-limit=4000000000>)

# ---------------------------------------------------------------------------
# unit tests for the library functions (incl. a function-level fuzzer)
# ---------------------------------------------------------------------------
add_executable(minijz_unit_tests tests/unit_tests.cpp)
target_include_directories(minijz_unit_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib)

enable_testing()

# ---------------------------------------------------------------------------
# constexpr proof checks: each MJZ_CX_CHECK=k is its own g++ invocation
# (g++ retains each constant expression's state until end-of-TU)
# ---------------------------------------------------------------------------
foreach(k ${MJZ_CX_CHECKS})
  add_test(NAME cx_check_${k}
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -O0
      -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/src
      -I${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib
      -DMJZ_CX_CHECK=${k}
      -fconstexpr-depth=8192 -fconstexpr-ops-limit=4000000000
      -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/constexpr_check.cpp
      -o ${CMAKE_CURRENT_BINARY_DIR}/cx${k}.o)
  set_tests_properties(cx_check_${k} PROPERTIES TIMEOUT 600 LABELS cx
    RUN_SERIAL TRUE) # each proof holds up to ~4 GiB of constexpr state
endforeach()

# ---------------------------------------------------------------------------
# differential fuzz tests: ctest -R fuzz  (seeds via -DFUZZ_SEEDS=start:count)
# ---------------------------------------------------------------------------
string(REGEX MATCH "^([0-9]+):([0-9]+)$" _fs "${FUZZ_SEEDS}")
if(NOT _fs)
  message(FATAL_ERROR "FUZZ_SEEDS must look like start:count (got ${FUZZ_SEEDS})")
endif()
set(_start ${CMAKE_MATCH_1})
set(_count ${CMAKE_MATCH_2})
math(EXPR _per "${_count} / ${FUZZ_SHARDS}")
math(EXPR _last "${_start} + ${_count} - 1")
set(_s ${_start})
set(_i 0)
while(_s LESS_EQUAL _last)
  math(EXPR _e "${_s} + ${_per} - 1")
  if(_e GREATER _last)
    set(_e ${_last})
  endif()
  math(EXPR _n "${_e} - ${_s} + 1")
  add_test(NAME fuzz_shard_${_i}
    COMMAND minijz --fuzz=${_n} --fuzz-from=${_s})
  set_tests_properties(fuzz_shard_${_i} PROPERTIES TIMEOUT 1800 LABELS fuzz)
  if(FUZZ_RANDOM_CONFIGS)
    add_test(NAME fuzz_random_shard_${_i}
      COMMAND minijz --fuzz=${_n} --fuzz-from=${_s} --fuzz-cfg=random)
    set_tests_properties(fuzz_random_shard_${_i} PROPERTIES TIMEOUT 1800 LABELS fuzz)
  endif()
  math(EXPR _s "${_e} + 1")
  math(EXPR _i "${_i} + 1")
endwhile()

# a native-parity fuzz gate (C+asm at -O0/-O2; proves the emitted code)
add_test(NAME fuzz_native_gate
  COMMAND minijz --fuzz=60 --fuzz-from=9001 --fuzz-native=8)
set_tests_properties(fuzz_native_gate PROPERTIES TIMEOUT 900 LABELS fuzz)

# selftest = demos + curated cx tours + fuzz(200)
add_test(NAME selftest COMMAND minijz --selftest)
set_tests_properties(selftest PROPERTIES TIMEOUT 900 LABELS fuzz)

add_test(NAME general_examples
  COMMAND minijz ${CMAKE_CURRENT_SOURCE_DIR}/examples/general.mjz
          --stdin=${CMAKE_CURRENT_SOURCE_DIR}/examples/general.in -O3)
set_tests_properties(general_examples PROPERTIES TIMEOUT 120 LABELS fuzz)

# ---------------------------------------------------------------------------
# regression gates (ctest -L regr): the historical bug repros (bug ledger 1-5,
# examples/BUGFIXES.md) through --check-all, the hand-written stress battery,
# and header self-containment (README: "not dependent on include order")
# ---------------------------------------------------------------------------
file(GLOB MJZ_BUGREPRO RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} bugrepro/*.mjz)
foreach(f ${MJZ_BUGREPRO})
  string(REGEX REPLACE "bugrepro/|\\.mjz$" "" nm ${f})
  add_test(NAME bugrepro_${nm}_O3
    COMMAND minijz ${CMAKE_CURRENT_SOURCE_DIR}/${f} --check-all -O3)
  set_tests_properties(bugrepro_${nm}_O3 PROPERTIES TIMEOUT 120 LABELS regr)
  add_test(NAME bugrepro_${nm}_O3_noscctco
    COMMAND minijz ${CMAKE_CURRENT_SOURCE_DIR}/${f} --check-all
            -O3 --disable=scctco)
  set_tests_properties(bugrepro_${nm}_O3_noscctco
    PROPERTIES TIMEOUT 120 LABELS regr)
endforeach()

add_test(NAME stress_battery
  COMMAND ${CMAKE_COMMAND} -E env MINIJZ_BIN=$<TARGET_FILE:minijz>
          bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/stress.sh)
set_tests_properties(stress_battery PROPERTIES TIMEOUT 1800 LABELS regr)

add_test(NAME header_order
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/header_order_test.sh)
set_tests_properties(header_order PROPERTIES TIMEOUT 900 LABELS regr)

# bug 6 (see examples/BUGFIXES.md §6): the exact pass configuration that
# hung the compiler — crossed phi-degenerate copies + copyprop, no dce
add_test(NAME bugrepro_s7406_hangcfg
  COMMAND minijz ${CMAKE_CURRENT_SOURCE_DIR}/bugrepro/s7406.mjz --check-all
          --stdin=${CMAKE_CURRENT_SOURCE_DIR}/bugrepro/s7406.in
          -O1 --disable=dce)
set_tests_properties(bugrepro_s7406_hangcfg PROPERTIES TIMEOUT 60 LABELS regr)

# ---------------------------------------------------------------------------
# showcases: build the showcase programs at several -O levels and keep a
# GIT SUBDIRECTORY in the build folder so successive builds can be diffed
# ("see the difference between different versions")
# ---------------------------------------------------------------------------
add_custom_target(showcases
  COMMAND ${CMAKE_COMMAND}
    -D BIN=$<TARGET_FILE:minijz>
    -D SRC=${CMAKE_CURRENT_SOURCE_DIR}
    -D DST=${CMAKE_CURRENT_BINARY_DIR}/showcases
    -D GIT=${GIT_EXECUTABLE}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_showcases.cmake
  DEPENDS minijz
  COMMENT "running showcases (general/showcase/demos at -O0..-O3) + git snapshot")
add_custom_target(showcase-diff
  COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}/showcases
          ${GIT_EXECUTABLE} diff --no-ext-diff HEAD^ HEAD --
  COMMENT "git diff of the last two showcase runs")
