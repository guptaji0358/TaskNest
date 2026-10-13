find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_tasknest tst_tasknest.cpp)

target_link_libraries(tst_tasknest PRIVATE
    tasknestcore
    Qt6::Test
)

add_test(NAME tasknest COMMAND tst_tasknest)
