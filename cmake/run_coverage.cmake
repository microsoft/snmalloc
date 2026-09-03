# Per-test cached coverage runner, invoked via `cmake -P`.
#
# For each registered ctest test we keep a subdirectory
# `${PROFROOT}/<NAME>/` containing that test's .profraw files plus a
# `.hash` file recording the sha256 of the binary that produced them.
# On rerun we recompute the hash and only re-execute tests whose
# binary changed (or whose subdir is missing). Stale tests are
# batched into a single `ctest -j -R` invocation so parallelism is
# preserved; the per-test ENVIRONMENT property set via
# set_tests_properties routes each test's profraws to its own subdir.
#
# Required cache variables (set on the cmake -P command line via -D):
#   PROFROOT      - directory holding per-test profile subdirs
#   PAIRS         - TSV file of "<NAME>\t<binary-path>" lines
#   CTEST         - path to ctest
#   LLVM_PROFDATA - path to llvm-profdata
#   LLVM_COV      - path to llvm-cov
#   COV_OBJECTS   - llvm-cov object args (first positional, rest -object ...)
#   COV_PROFDATA  - output .profdata path
#   COV_JSON      - output .json path
#   COV_REPORT    - output .report.txt path
#   CTEST_JOBS    - parallelism for ctest

foreach(var PROFROOT PAIRS CTEST LLVM_PROFDATA LLVM_COV COV_OBJECTS
            COV_PROFDATA COV_JSON COV_REPORT CTEST_JOBS)
  if (NOT DEFINED ${var})
    message(FATAL_ERROR "run_coverage.cmake: ${var} is not set")
  endif()
endforeach()

file(MAKE_DIRECTORY "${PROFROOT}")

file(READ "${PAIRS}" _pairs_text)
string(REPLACE "\n" ";" _pair_lines "${_pairs_text}")

set(_stale "")
set(_ran 0)
set(_skipped 0)

foreach(_line IN LISTS _pair_lines)
  if (_line STREQUAL "")
    continue()
  endif()
  # Split on tab into NAME and BIN.
  string(REPLACE "\t" ";" _fields "${_line}")
  list(LENGTH _fields _n)
  if (NOT _n EQUAL 2)
    message(WARNING "run_coverage.cmake: skipping malformed line: ${_line}")
    continue()
  endif()
  list(GET _fields 0 _name)
  list(GET _fields 1 _bin)

  set(_tdir "${PROFROOT}/${_name}")
  set(_hashfile "${_tdir}/.hash")

  set(_cur "")
  if (EXISTS "${_bin}")
    file(SHA256 "${_bin}" _cur)
  endif()

  set(_prev "")
  if (EXISTS "${_hashfile}")
    file(READ "${_hashfile}" _prev)
    string(STRIP "${_prev}" _prev)
  endif()

  if (NOT _cur STREQUAL "" AND _cur STREQUAL _prev)
    math(EXPR _skipped "${_skipped} + 1")
    continue()
  endif()

  # Re-run this test: clear its profile dir, but defer writing the
  # hash until after ctest succeeds (so a test that produces no
  # .profraw — crash, timeout, missing test — is retried next run
  # rather than silently cached as "covered").
  file(REMOVE_RECURSE "${_tdir}")
  file(MAKE_DIRECTORY "${_tdir}")
  list(APPEND _stale "${_name}")
  list(APPEND _stale_hashes "${_name}=${_cur}")
  math(EXPR _ran "${_ran} + 1")
endforeach()

if (_stale)
  list(JOIN _stale "|" _stale_re)
  execute_process(
    COMMAND "${CTEST}" -j ${CTEST_JOBS} -R "^(${_stale_re})\$"
            --output-on-failure --timeout 300
    RESULT_VARIABLE _ctest_rc)
  # Tolerate test failures so we still merge whatever profile data
  # was produced — but only cache hashes when ctest reported overall
  # success. A failed/timed-out test may have flushed a *partial*
  # .profraw, and caching the hash would freeze that partial data
  # as the canonical answer for this binary. Retry next run instead.
  if (NOT _ctest_rc EQUAL 0)
    message(STATUS
      "coverage: ctest reported failures (rc=${_ctest_rc}); "
      "not caching hashes for any rerun test")
  else()
    foreach(_entry IN LISTS _stale_hashes)
      string(REPLACE "=" ";" _kv "${_entry}")
      list(GET _kv 0 _name)
      list(GET _kv 1 _hash)
      set(_tdir "${PROFROOT}/${_name}")
      file(GLOB _produced "${_tdir}/*.profraw")
      if (_produced)
        file(WRITE "${_tdir}/.hash" "${_hash}\n")
      else()
        message(STATUS
          "coverage: ${_name} produced no .profraw (will retry next run)")
      endif()
    endforeach()
  endif()
endif()

message(STATUS "coverage: ran=${_ran} skipped(cached)=${_skipped}")

file(GLOB_RECURSE _profraws "${PROFROOT}/*.profraw")
if (NOT _profraws)
  message(FATAL_ERROR "coverage: no .profraw files found under ${PROFROOT}")
endif()

execute_process(
  COMMAND "${LLVM_PROFDATA}" merge -sparse ${_profraws} -o "${COV_PROFDATA}"
  RESULT_VARIABLE _rc)
if (NOT _rc EQUAL 0)
  message(FATAL_ERROR "llvm-profdata merge failed (exit ${_rc})")
endif()

# COV_OBJECTS is a single string with the first object positional and
# subsequent objects prefixed by "-object". Split it back into a list
# for execute_process.
separate_arguments(_cov_objects_list UNIX_COMMAND "${COV_OBJECTS}")

execute_process(
  COMMAND "${LLVM_COV}" report -instr-profile=${COV_PROFDATA} ${_cov_objects_list}
  OUTPUT_FILE "${COV_REPORT}"
  RESULT_VARIABLE _rc)
if (NOT _rc EQUAL 0)
  message(FATAL_ERROR "llvm-cov report failed (exit ${_rc})")
endif()

execute_process(
  COMMAND "${LLVM_COV}" export -instr-profile=${COV_PROFDATA} ${_cov_objects_list}
  OUTPUT_FILE "${COV_JSON}"
  RESULT_VARIABLE _rc)
if (NOT _rc EQUAL 0)
  message(FATAL_ERROR "llvm-cov export failed (exit ${_rc})")
endif()

message(STATUS "Coverage written to ${COV_PROFDATA} / ${COV_JSON} / ${COV_REPORT}")
