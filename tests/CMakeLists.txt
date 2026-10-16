find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_tasknest tst_tasknest.cpp)
target_compile_definitions(tst_tasknest PRIVATE
    TASKNEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(tst_tasknest PRIVATE
    tasknestcore
    Qt6::Test
)

# Skipped tests are not failures to Qt Test, but a run that skipped tests is not
# full verification. RunQtTest.cmake prints the PASS/FAIL/SKIP summary and fails
# the entry as INCOMPLETE when more than the budget was skipped.
set(TASKNEST_SKIP_BUDGET 0 CACHE STRING "Tests allowed to be skipped in tst_tasknest before the run counts as incomplete")
# Helper programs/executables the tests start as separate, disposable processes.
add_executable(tst_lockowner tst_lockowner.cpp)
target_link_libraries(tst_lockowner PRIVATE Qt6::Core)
add_dependencies(tst_tasknest tst_lockowner appTaskNest TaskNestHost)
target_compile_definitions(tst_tasknest PRIVATE
    TASKNEST_LOCKOWNER_EXE="$<TARGET_FILE:tst_lockowner>"
    TASKNEST_APP_EXE="$<TARGET_FILE:appTaskNest>"
    TASKNEST_HOST_EXE="$<TARGET_FILE:TaskNestHost>"
)

add_test(NAME tasknest
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:tst_tasknest> -DNAME=tasknest
            -DLOG=${CMAKE_CURRENT_BINARY_DIR}/tst_tasknest.log -DSKIP_BUDGET=${TASKNEST_SKIP_BUDGET}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/RunQtTest.cmake)

# UI tests: drive the real QML (scripts/main/MainView.qml) with Qt Quick Test.
find_package(Qt6 REQUIRED COMPONENTS QuickTest Quick QuickControls2)

# The application's Qt resource file (icon), so MainView finds its image.
qt_add_resources(TST_UI_RESOURCES ${CMAKE_SOURCE_DIR}/resources/app.qrc)
qt_add_executable(tst_ui ui_main.cpp ${TST_UI_RESOURCES})

target_compile_definitions(tst_ui PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
)

target_link_libraries(tst_ui PRIVATE
    tasknestcore
    Qt6::QuickTest
    Qt6::Quick
    Qt6::QuickControls2
)

# Keyboard-driven UI tests need OS foreground activation; a test that positively
# detects it cannot get it skips with a visible reason. With the default budget
# of 0 any skip makes this entry fail as INCOMPLETE; raise the cache variable
# only to accept a known, reported amount of blocked coverage.
set(TASKNEST_UI_SKIP_BUDGET 0 CACHE STRING "Tests allowed to be skipped in tst_ui before the run counts as incomplete")
add_test(NAME tasknest_ui
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:tst_ui> -DNAME=tasknest_ui
            -DLOG=${CMAKE_CURRENT_BINARY_DIR}/tst_ui.log -DSKIP_BUDGET=${TASKNEST_UI_SKIP_BUDGET}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/RunQtTest.cmake)

# The wrapper itself is tested with fake test executables: it must fail on real
# failures, on exceeded skip budgets, and on missing/truncated/empty output.
add_executable(tst_fakeqt tst_fakeqt.cpp)
add_test(NAME qt_test_wrapper
    COMMAND ${CMAKE_COMMAND} -DWRAPPER=${CMAKE_CURRENT_SOURCE_DIR}/RunQtTest.cmake
            -DFAKE=$<TARGET_FILE:tst_fakeqt>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/wrapper_selftest
            -P ${CMAKE_CURRENT_SOURCE_DIR}/RunQtTestSelfTest.cmake)
