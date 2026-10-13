cmake_minimum_required(VERSION 3.21)

project(TaskNest VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt is located through CMAKE_PREFIX_PATH (pass -DCMAKE_PREFIX_PATH=<Qt kit dir>).
find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Sql)

qt_standard_project_setup(REQUIRES 6.5)

# Data layer (database, tasks), shared by the application and the tests.
qt_add_library(tasknestcore STATIC
    scripts/src/database.cpp
    scripts/src/taskrepository.cpp
    scripts/src/tasklistmodel.cpp
    scripts/include/database.h
    scripts/include/task.h
    scripts/include/taskrepository.h
    scripts/include/tasklistmodel.h
)

target_include_directories(tasknestcore PUBLIC
    "${CMAKE_SOURCE_DIR}/scripts/include"
)

target_link_libraries(tasknestcore PUBLIC
    Qt6::Core
    Qt6::Sql
)

qt_add_executable(appTaskNest
    scripts/main/main.cpp
)

set_target_properties(appTaskNest PROPERTIES
    OUTPUT_NAME TaskNest
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/scripts/main/bin/$<CONFIG>"
)

# Generate qmldir files for QML living outside the source root (scripts/main).
qt_policy(SET QTP0004 NEW)

qt_add_qml_module(appTaskNest
    URI TaskNest
    VERSION 1.0
    QML_FILES
        scripts/main/Main.qml
)

target_link_libraries(appTaskNest PRIVATE
    tasknestcore
    Qt6::Quick
    Qt6::QuickControls2
)

option(TASKNEST_BUILD_TESTS "Build the TaskNest automated tests" ON)
if(TASKNEST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
