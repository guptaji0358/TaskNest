cmake_minimum_required(VERSION 3.21)

project(TaskNest VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt is located through CMAKE_PREFIX_PATH (pass -DCMAKE_PREFIX_PATH=<Qt kit dir>).
find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Sql)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(appTaskNest
    scripts/main/main.cpp
    scripts/src/database.cpp
    scripts/include/database.h
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

target_include_directories(appTaskNest PRIVATE
    "${CMAKE_SOURCE_DIR}/scripts/include"
)

target_link_libraries(appTaskNest PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Sql
)
