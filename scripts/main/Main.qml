import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root

    // Set from main.cpp before the window is created.
    required property string databaseError
    required property string databasePath
    required property var taskModel // null when the database is unavailable
    property var reminderScheduler: null
    property var widgetController: null // WidgetController; null when the database is unavailable
    property var shortcutManager: null // ShortcutManager
    property bool reducedMotion: false // Windows "animation effects" off

    width: 1040
    height: 700
    minimumWidth: 800
    minimumHeight: 520
    visible: true
    title: "TaskNest"
    color: Theme.bg

    Component.onCompleted: Theme.reducedMotion = reducedMotion

    // The widget is a separate window; when this one closes the application
    // ends (preferences are written first), so nothing is left running.
    onClosing: {
        if (widgetController)
            widgetController.prepareForShutdown()
        Qt.quit()
    }

    function bringToFront() {
        if (visibility === Window.Minimized)
            showNormal()
        show()
        raise()
        requestActivate()
    }

    // Created only when there is a usable database (and so a controller).
    Loader {
        id: widgetLoader
        active: root.widgetController !== null && root.taskModel !== null
        sourceComponent: FloatingWidget {
            controller: root.widgetController
            taskModel: root.taskModel
            remindersModel: mainView.reminderList
            onOpenTaskRequested: (taskId) => {
                root.bringToFront()
                mainView.openTask(taskId)
            }
            onOpenAppRequested: root.bringToFront()
            onReminderDoneRequested: (taskId) => mainView.dismissReminder(taskId, true)
            onReminderDismissRequested: (taskId) => mainView.dismissReminder(taskId, false)
        }
    }

    MainView {
        id: mainView
        widgetController: root.widgetController
        shortcutManager: root.shortcutManager
        anchors.fill: parent
        databaseError: root.databaseError
        databasePath: root.databasePath
        taskModel: root.taskModel
        reminderScheduler: root.reminderScheduler
        onReminderArrived: root.alert(0) // flash the taskbar button until focused
    }
}
