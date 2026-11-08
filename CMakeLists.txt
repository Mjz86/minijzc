# see CMakePresets.json for ready-made configurations ("clang", "fancy",
# "gcc", "header-only"); plain `cmake -S . -B build` = multi-TU + whatever
# C++ compiler the environment provides (clang++ preferred when unpinned).
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# compiler preference: unless the user pinned one (-DCMAKE_CXX_COMPILER=...,
# $CXX, or a toolchain file), prefer clang++ (README note: "make cmake
# detect and use clang"), falling back to the platform default (g++ / MSVC)
# ---------------------------------------------------------------------------
find_program(MJZ_CLANGXX NAMES clang++ clang++-20 clang++-19 clang++-18
                                 clang++-17 clang++-16)
if(MJZ_CLANGXX AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX}
   AND NOT DEFINED ENV{CXXFLAGS})
  set(CMAKE_CXX_COMPILER "${MJZ_CLANGXX}" CACHE FILEPATH "C++ compiler")
  message(STATUS "minijzc: no compiler pinned -> clang++ (${MJZ_CLANGXX});"
                 " override with -DCMAKE_CXX_COMPILER=... if unwanted")
endif()

project(minijzc CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MINIJZ_CXX_STANDARD "20" CACHE STRING "C++ standard (20 or 26)")
if(NOT MINIJZ_CXX_STANDARD MATCHES "^(20|23|26|2b)$")
  message(FATAL_ERROR "MINIJZ_CXX_STANDARD must be 20 or 26 (got ${MINIJZ_CXX_STANDARD})")
endif()
set(CMAKE_CXX_STANDARD ${MINIJZ_CXX_STANDARD})
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# g++ and clang++ spell the constexpr budget differently (README notes):
# per-proof limits, applied wherever the cx checks are compiled
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(MJZ_CX_LIMITS -fconstexpr-steps=4000000000)
else()
  set(MJZ_CX_LIMITS -fconstexpr-depth=8192 -fconstexpr-ops-limit=4000000000)
endif()

# ---------------------------------------------------------------------------
# options
# ---------------------------------------------------------------------------
option(MINIJZ_MULTI_CPP
  "ON: compile the compiler as 9 REAL translation units (a thin main that
       only parses options + one TU per subsystem behind light headers;
       parallel + incremental builds). OFF: single translation unit (the
       header-only constexpr form)." ON)
option(MINIJZ_EXT_WARNINGS
  "the extended warning battery (README's fancy clang++ line, mapped per
   compiler: -Wshadow -Wsign-conversion -Wcast-qual ... )." OFF)
option(MINIJZ_WERROR "treat warnings as errors" OFF)
option(MINIJZ_NATIVE_TUNING "compile with -march=native" OFF)
set(FUZZ_SEEDS "1:400" CACHE STRING
  "fuzz seed window for ctest, 'start:count' (e.g. 4001:400, 1:2000)")
set(FUZZ_SHARDS 1 CACHE STRING "split FUZZ_SEEDS into this many ctest cases")
set(FUZZ_JOBS 1 CACHE STRING "parallel fuzz workers for the ctest gates")
option(FUZZ_RANDOM_CONFIGS ON "also fuzz with random pass configurations")
set(MJZ_CX_CHECKS "1;2;3;4;5;6" CACHE STRING "constexpr proof check numbers")

# the mjz_lib clone next to this repo (README: git clone String mjz_lib)
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib/alllib_import.hpp)
  message(FATAL_ERROR
    "mjz_lib not found: 'git clone https://github.com/Mjz86/String mjz_lib'"
    " inside this repo first (see README.md)")
endif()

# ---------------------------------------------------------------------------
# warning/tuning flags (kept OUT of the default build: the battery must
# stay reproducible; opt in via preset "fancy" or -DMINIJZ_EXT_WARNINGS=ON)
# ---------------------------------------------------------------------------
set(MJZ_EXTRA_FLAGS "")
if(MINIJZ_EXT_WARNINGS)
  list(APPEND MJZ_EXTRA_FLAGS
    -Wall -Wextra -Walloca -Wcast-align -Wcast-qual -Wctor-dtor-privacy
    -Wdeprecated-copy-dtor -Wdouble-promotion -Wenum-conversion
    -Wfloat-equal -Wformat=2 -Wmultichar -Wnon-virtual-dtor
    -Woverloaded-virtual -Wpointer-arith -Wrange-loop-construct -Wshadow
    -Wuninitialized -Wvla -Wwrite-strings -Wsign-conversion
    -Wdelete-non-virtual-dtor)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND MJZ_EXTRA_FLAGS -Wmismatched-tags -ferror-limit=30)
  else()
    list(APPEND MJZ_EXTRA_FLAGS -Wformat-signedness -fmax-errors=30)
  endif()
endif()
if(MINIJZ_NATIVE_TUNING)
  list(APPEND MJZ_EXTRA_FLAGS -march=native)
endif()
if(MINIJZ_WERROR)
  list(APPEND MJZ_EXTRA_FLAGS -Werror)
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
else()
  add_executable(minijz main.cpp)
endif()
target_include_directories(minijz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                         ${CMAKE_CURRENT_SOURCE_DIR}/src
                                         ${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib)
target_compile_options(minijz PRIVATE ${MJZ_EXTRA_FLAGS}
  $<$<CONFIG:Debug>:${MJZ_CX_LIMITS}>)

# ---------------------------------------------------------------------------
# unit tests for the library functions (incl. a function-level fuzzer)
# ---------------------------------------------------------------------------
add_executable(minijz_unit_tests tests/unit_tests.cpp)
target_include_directories(minijz_unit_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib)
target_compile_options(minijz_unit_tests PRIVATE ${MJZ_EXTRA_FLAGS})

enable_testing()

# ---------------------------------------------------------------------------
# constexpr proof checks: each MJZ_CX_CHECK=k is its own compiler invocation
# (each proof retains its full constant-expression state until end-of-TU)
# ---------------------------------------------------------------------------
foreach(k ${MJZ_CX_CHECKS})
  add_test(NAME cx_check_${k}
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++${MINIJZ_CXX_STANDARD} -O0
      -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/src
      -I${CMAKE_CURRENT_SOURCE_DIR}/mjz_lib
      -DMJZ_CX_CHECK=${k} ${MJZ_CX_LIMITS}
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
    COMMAND minijz --fuzz=${_n} --fuzz-from=${_s} --fuzz-jobs=${FUZZ_JOBS})
  set_tests_properties(fuzz_shard_${_i} PROPERTIES TIMEOUT 1800 LABELS fuzz)
  if(FUZZ_RANDOM_CONFIGS)
    add_test(NAME fuzz_random_shard_${_i}
      COMMAND minijz --fuzz=${_n} --fuzz-from=${_s} --fuzz-cfg=random
              --fuzz-jobs=${FUZZ_JOBS})
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
# regression gates (ctest -L regr): the historical bug repros (bug ledger 1-6,
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

# a scripted --debugger session: the interactive facility is itself a
# regression gate (IR dump, single-step, full state, five-way check)
add_test(NAME debugger_session
  COMMAND bash -c
    "printf 'fns\nvm reset\nvm step 5\nvm state\nvm run\ncheck\nquit\n' |
     '$<TARGET_FILE:minijz>' ${CMAKE_CURRENT_SOURCE_DIR}/demos/tco_parity.mjz
       --debugger --stdin=${CMAKE_CURRENT_SOURCE_DIR}/examples/general.in")
set_tests_properties(debugger_session PROPERTIES TIMEOUT 60 LABELS regr)

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

# ---------------------------------------------------------------------------
# clang-format targets (README note: keep the formatting consistent)
# ---------------------------------------------------------------------------
find_program(CLANG_FORMAT NAMES clang-format clang-format-19 clang-format-18)
if(CLANG_FORMAT)
  file(GLOB_RECURSE MJZ_FMT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i ${MJZ_FMT_SOURCES}
    COMMENT "clang-format -i over the compiler sources")
  add_custom_target(check-format
    COMMAND ${CLANG_FORMAT} --dry-run -Werror ${MJZ_FMT_SOURCES}
    COMMENT "verify formatting (fails on drift)")
else()
  message(STATUS "clang-format not found: 'format'/'check-format' targets disabled"
                 " (pip install clang-format)")
endif()

# ---------------------------------------------------------------------------
# a friendlier hint when the toolchain is missing (the Windows nmake trap:
# pass -G "MinGW Makefiles" (or "Ninja") and let the clang preference above
# find clang++, instead of failing on a missing MSVC)
# ---------------------------------------------------------------------------
if(WIN32 AND NOT CMAKE_CXX_COMPILER)
  message(WARNING "no C++ compiler detected: install LLVM/MinGW and retry"
                  " with -G \"MinGW Makefiles\" -DCMAKE_CXX_COMPILER=clang++")
endif()
