cmake_minimum_required(VERSION 3.21)

project(TaskNest VERSION 0.1.0 LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()

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
find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Sql Network)

qt_standard_project_setup(REQUIRES 6.5)

# Integration client and native-messaging bridge: Qt Core + Network only, so the
# companion host (TaskNestHost.exe) does not depend on the rest of the app.
qt_add_library(tasknesthostcore STATIC
    scripts/src/integrationclient.cpp
    scripts/src/nativehost.cpp
    scripts/src/testprofile.cpp
    scripts/include/integrationclient.h
    scripts/include/nativehost.h
    scripts/include/testprofile.h
)
target_include_directories(tasknesthostcore PUBLIC "${CMAKE_SOURCE_DIR}/scripts/include")
target_link_libraries(tasknesthostcore PUBLIC Qt6::Core Qt6::Network)

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
    scripts/src/integrationserver.cpp
    scripts/src/selectioncapture.cpp
    scripts/include/selectioncapture.h
    scripts/src/deeplink.cpp
    scripts/src/globalhotkey.cpp
    scripts/include/deeplink.h
    scripts/include/globalhotkey.h
    scripts/include/integrationserver.h
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
    tasknesthostcore
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Network
)

# Icon resources: the Qt resource file feeds the window icon; the Windows
# resource file embeds the executable icon and version info.
qt_add_resources(TASKNEST_RESOURCES resources/app.qrc)
set(TASKNEST_WIN_RC "")
if(WIN32)
    # windres mishandles -I paths containing spaces, and the icon is the only
    # thing the resource file includes: drop the include flags and point the
    # generated copy of app.rc at the icon by absolute path.
    set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -O coff <DEFINES> <FLAGS> <SOURCE> <OBJECT>")
    file(READ resources/app.rc TASKNEST_RC_TEXT)
    string(REPLACE "\"app.ico\"" "\"${CMAKE_SOURCE_DIR}/resources/app.ico\"" TASKNEST_RC_TEXT "${TASKNEST_RC_TEXT}")
    file(WRITE "${CMAKE_BINARY_DIR}/app.rc" "${TASKNEST_RC_TEXT}")
    set(TASKNEST_WIN_RC "${CMAKE_BINARY_DIR}/app.rc")
    set_source_files_properties("${CMAKE_BINARY_DIR}/app.rc" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/resources/app.ico")
endif()

qt_add_executable(appTaskNest
    scripts/main/main.cpp
    ${TASKNEST_RESOURCES}
    ${TASKNEST_WIN_RC}
)

# Companion console program started by Chrome/Edge for the browser extension.
# It answers one message and exits; it never starts TaskNest.
qt_add_executable(TaskNestHost scripts/main/hostmain.cpp)
target_link_libraries(TaskNestHost PRIVATE tasknesthostcore)
set_target_properties(TaskNestHost PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/bin")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(TaskNestHost PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_SOURCE_DIR}/out/bin")
endforeach()

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

# Copy the Qt and MinGW runtime DLLs, plugins and QML modules next to the
# executables so they start without Qt/MinGW on PATH (and cannot pick up a
# libstdc++ from another toolchain). Everything lands in out/bin (git-ignored);
# out/bin/data is never touched. Turn off with -DTASKNEST_DEPLOY_DLLS=OFF.
option(TASKNEST_DEPLOY_DLLS "Run windeployqt after building TaskNest" ON)
if(WIN32 AND TASKNEST_DEPLOY_DLLS)
    get_target_property(_tn_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_tn_qt_bin "${_tn_qmake}" DIRECTORY)
    find_program(TASKNEST_WINDEPLOYQT windeployqt
        HINTS "${_tn_qt_bin}" NO_DEFAULT_PATH)
    if(TASKNEST_WINDEPLOYQT)
        add_custom_command(TARGET appTaskNest POST_BUILD
            COMMAND "${TASKNEST_WINDEPLOYQT}"
                --no-translations --no-system-d3d-compiler --no-opengl-sw
                --qmldir "${CMAKE_SOURCE_DIR}/scripts/main"
                "$<TARGET_FILE:appTaskNest>"
            COMMENT "Deploying Qt/MinGW runtime into out/bin"
            VERBATIM)
    else()
        message(WARNING "windeployqt not found; runtime DLLs will not be copied")
    endif()
endif()

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
        scripts/main/Atmosphere.qml
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
        scripts/main/TNPopupSurface.qml
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
        scripts/main/QuickCaptureWindow.qml
        scripts/main/SelectionCaptureWindow.qml
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
