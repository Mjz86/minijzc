# run_showcases.cmake — invoked by the `showcases` target.
if(NOT DEFINED GIT OR GIT STREQUAL "")
  set(GIT_EXECUTABLE "git")
else()
  set(GIT_EXECUTABLE "${GIT}")
endif()
# Builds the showcase programs at -O0..-O3, stores one text file per
# (program, level), then commits everything into the git repository that
# lives in the build/showcases directory ("a git subdirectory in the build
# folder"), so `showcase-diff` / `git log` show the difference between
# versions of the compiler.
file(MAKE_DIRECTORY "${DST}")
set(_have_git TRUE)
if(NOT EXISTS "${DST}/.git")
  execute_process(COMMAND ${GIT_EXECUTABLE} init WORKING_DIRECTORY "${DST}"
                  RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    set(_have_git FALSE)
    message(STATUS "git unavailable: showcases saved without version history")
  else()
    execute_process(COMMAND ${GIT_EXECUTABLE} config user.email showcase@build
                    WORKING_DIRECTORY "${DST}")
    execute_process(COMMAND ${GIT_EXECUTABLE} config user.name showcase
                    WORKING_DIRECTORY "${DST}")
  endif()
endif()

set(_cases "") # name|file|stdin
list(APPEND _cases "general|${SRC}/examples/general.mjz|${SRC}/examples/general.in")
list(APPEND _cases "showcase|${SRC}/examples/showcase.mjz|")
file(GLOB _demos "${SRC}/demos/*.mjz")
foreach(_d ${_demos})
  get_filename_component(_n "${_d}" NAME_WE)
  list(APPEND _cases "${_n}|${_d}|")
endforeach()

foreach(_case ${_cases})
  string(REGEX REPLACE "^([^|]*)\\|([^|]*)\\|(.*)$" "\\1" _name "${_case}")
  string(REGEX REPLACE "^([^|]*)\\|([^|]*)\\|(.*)$" "\\2" _file "${_case}")
  string(REGEX REPLACE "^([^|]*)\\|([^|]*)\\|(.*)$" "\\3" _stdin "${_case}")
  foreach(_lvl 0 1 2 3)
    set(_out "")
    if(_stdin STREQUAL "")
      execute_process(COMMAND ${BIN} ${_file} -O${_lvl}
                      OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
    else()
      execute_process(COMMAND ${BIN} ${_file} --stdin=${_stdin} -O${_lvl}
                      OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
    endif()
    if(NOT _rc EQUAL 0)
      set(_out "EXIT=${_rc}\n${_out}")
    endif()
    file(WRITE "${DST}/${_name}.O${_lvl}.txt" "${_out}")
  endforeach()
endforeach()

# the -O3 pass-statistics report: the interesting "difference between
# versions" usually shows up in these counts
execute_process(COMMAND ${BIN} ${SRC}/examples/general.mjz --stdin=${SRC}/examples/general.in -O3
                OUTPUT_VARIABLE _rep)
file(WRITE "${DST}/report.txt" "${_rep}")
if(_have_git)
  execute_process(COMMAND ${GIT_EXECUTABLE} add -A WORKING_DIRECTORY "${DST}")
  execute_process(COMMAND ${GIT_EXECUTABLE} commit -q -m "showcase run"
                  WORKING_DIRECTORY "${DST}" OUTPUT_QUIET ERROR_QUIET)
endif()
message(STATUS "showcases updated in ${DST} (git log / cmake --build build --target showcase-diff)")
