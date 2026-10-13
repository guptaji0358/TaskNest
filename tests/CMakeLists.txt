find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_tasknest tst_tasknest.cpp)

target_link_libraries(tst_tasknest PRIVATE
    tasknestcore
    Qt6::Test
)

add_test(NAME tasknest COMMAND tst_tasknest)

# UI tests: drive the real QML (scripts/main/MainView.qml) with Qt Quick Test.
find_package(Qt6 REQUIRED COMPONENTS QuickTest Quick QuickControls2)

qt_add_executable(tst_ui ui_main.cpp)

target_compile_definitions(tst_ui PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
)

target_link_libraries(tst_ui PRIVATE
    tasknestcore
    Qt6::QuickTest
    Qt6::Quick
    Qt6::QuickControls2
)

add_test(NAME tasknest_ui COMMAND tst_ui)
