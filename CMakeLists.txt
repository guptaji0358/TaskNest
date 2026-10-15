cmake_minimum_required(VERSION 3.21)

project(TaskNest VERSION 0.1.0 LANGUAGES CXX)

# One build configuration, one executable: out/bin/TaskNest.exe. Release is the
# default; pass -DCMAKE_BUILD_TYPE=Debug to the same build directory to rebuild
# that same file with debug information.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt is located through CMAKE_PREFIX_PATH (pass -DCMAKE_PREFIX_PATH=<Qt kit dir>).
find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Sql)

qt_standard_project_setup(REQUIRES 6.5)

# Data layer (database, tasks), shared by the application and the tests.
qt_add_library(tasknestcore STATIC
    scripts/src/cleanuppolicy.cpp
    scripts/src/database.cpp
    scripts/src/taskrepository.cpp
    scripts/src/tasklistmodel.cpp
    scripts/src/widgetcontroller.cpp
    scripts/src/reminderscheduler.cpp
    scripts/src/singleinstance.cpp
    scripts/src/shortcutmanager.cpp
    scripts/include/shortcutmanager.h
    scripts/include/cleanuppolicy.h
    scripts/include/database.h
    scripts/include/task.h
    scripts/include/taskrepository.h
    scripts/include/tasklistmodel.h
    scripts/include/widgetcontroller.h
    scripts/include/reminderscheduler.h
    scripts/include/singleinstance.h
)

target_include_directories(tasknestcore PUBLIC
    "${CMAKE_SOURCE_DIR}/scripts/include"
)

target_link_libraries(tasknestcore PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
)

qt_add_executable(appTaskNest
    scripts/main/main.cpp
)

# The application executable lives in one fixed place, whatever the configuration.
set(TASKNEST_BIN_DIR "${CMAKE_SOURCE_DIR}/out/bin")

set_target_properties(appTaskNest PROPERTIES
    OUTPUT_NAME TaskNest
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY "${TASKNEST_BIN_DIR}"
)
# Multi-config generators would otherwise add a per-config subfolder.
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(appTaskNest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${TASKNEST_BIN_DIR}")
endforeach()

# Generate qmldir files for QML living outside the source root (scripts/main).
qt_policy(SET QTP0004 NEW)

# Theme is the design-system singleton shared by every component.
set_source_files_properties(scripts/main/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(appTaskNest
    URI TaskNest
    VERSION 1.0
    QML_FILES
        scripts/main/Theme.qml
        scripts/main/Icon.qml
        scripts/main/TNLabel.qml
        scripts/main/TNButton.qml
        scripts/main/TNIconButton.qml
        scripts/main/TNTextField.qml
        scripts/main/TNComboBox.qml
        scripts/main/TNScrollBar.qml
        scripts/main/DateTimeInput.js
        scripts/main/TNSpinBox.qml
        scripts/main/TNCalendar.qml
        scripts/main/TNDateField.qml
        scripts/main/TNTimeField.qml
        scripts/main/TNDialog.qml
        scripts/main/TNToast.qml
        scripts/main/QuickAddPanel.qml
        scripts/main/TaskCheck.qml
        scripts/main/NavItem.qml
        scripts/main/TaskRow.qml
        scripts/main/ReminderBanner.qml
        scripts/main/EmptyState.qml
        scripts/main/TaskEditor.qml
        scripts/main/CleanupSettings.qml
        scripts/main/FloatingWidget.qml
        scripts/main/MainView.qml
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
