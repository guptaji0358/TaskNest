# Runs one Qt Test executable and reports its result honestly.
#
# The test executables use the Windows GUI subsystem, so CTest sees no stdout and
# a plain add_test() would hide skips. This script runs the executable with a
# log file, prints a PASS/FAIL/SKIP summary (and every skipped/failed test) to
# CTest's output, and fails the CTest entry when
#   * the executable fails, the log is missing, has no "Totals:" line (crashed or
#     truncated) or reports zero tests, or
#   * more than SKIP_BUDGET tests were skipped: the run is then INCOMPLETE and
#     must not be read as full verification.
#
# Inputs: -DEXE=<test exe> -DLOG=<log file> -DNAME=<label> -DSKIP_BUDGET=<n>
if(NOT DEFINED SKIP_BUDGET)
    set(SKIP_BUDGET 0)
endif()

file(REMOVE "${LOG}")
execute_process(COMMAND "${EXE}" -o "${LOG},txt"
                RESULT_VARIABLE exit_code
                OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)

if(NOT EXISTS "${LOG}")
    message(FATAL_ERROR "${NAME}: no log was written (exit code ${exit_code}).")
endif()
file(READ "${LOG}" log)
string(REGEX MATCH "Totals: ([0-9]+) passed, ([0-9]+) failed, ([0-9]+) skipped" totals "${log}")
if(NOT totals)
    message(FATAL_ERROR "${NAME}: the run did not finish (no Totals line; exit code ${exit_code}). See ${LOG}")
endif()
set(passed ${CMAKE_MATCH_1})
set(failed ${CMAKE_MATCH_2})
set(skipped ${CMAKE_MATCH_3})
math(EXPR total "${passed} + ${failed} + ${skipped}")
if(total EQUAL 0)
    message(FATAL_ERROR "${NAME}: no tests ran (Totals line reports zero tests). See ${LOG}")
endif()

string(REGEX MATCHALL "(^|\n)(FAIL!|SKIP)[^\n]*" notable "${log}")
foreach(line IN LISTS notable)
    string(STRIP "${line}" line)
    message(STATUS "${line}")
endforeach()
message(STATUS "${NAME} SUMMARY: ${passed} passed, ${failed} failed, ${skipped} skipped (skip budget ${SKIP_BUDGET}); log: ${LOG}")

if(failed GREATER 0 OR NOT exit_code EQUAL failed)
    message(FATAL_ERROR "${NAME}: ${failed} test(s) failed (exit code ${exit_code}).")
endif()
if(skipped GREATER SKIP_BUDGET)
    message(FATAL_ERROR "${NAME}: INCOMPLETE - ${skipped} test(s) were skipped (budget ${SKIP_BUDGET}), "
                        "so this run is not full verification. Skips are listed above; they happen when "
                        "the test window cannot get OS foreground activation.")
endif()
