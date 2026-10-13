import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root

    // Set from main.cpp before the window is created.
    required property string databaseError
    required property string databasePath
    required property var taskModel // null when the database is unavailable
    property var reminderScheduler: null
    property bool reducedMotion: false // Windows "animation effects" off

    width: 1040
    height: 700
    minimumWidth: 800
    minimumHeight: 520
    visible: true
    title: "TaskNest"
    color: Theme.bg

    Component.onCompleted: Theme.reducedMotion = reducedMotion

    MainView {
        anchors.fill: parent
        databaseError: root.databaseError
        databasePath: root.databasePath
        taskModel: root.taskModel
        reminderScheduler: root.reminderScheduler
        onReminderArrived: root.alert(0) // flash the taskbar button until focused
    }
}
