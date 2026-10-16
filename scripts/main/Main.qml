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
    property var integrationServer: null // IntegrationServer; null when the database is unavailable
    property var deepLinkRouter: null    // DeepLinkRouter
    property var globalHotkey: null      // GlobalHotkey; null when the database is unavailable
    property var captureHotkey: null     // GlobalHotkey for "capture selection"
    property var selectionCapture: null  // SelectionCapture
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

    // Requests from the local integration endpoint. They raise the main window
    // only; the floating widget is never opened by them.
    Connections {
        target: root.integrationServer
        ignoreUnknownSignals: true
        function onOpenViewRequested(view) { root.bringToFront(); mainView.openView(view) }
        function onOpenTaskRequested(taskId) { root.bringToFront(); mainView.openTask(taskId) }
        function onQuickAddRequested() { root.bringToFront(); mainView.openQuickAdd() }
    }

    // tasknest:// links. Views and tasks raise the main window; Quick Add uses
    // the compact capture window and leaves the main window alone.
    Connections {
        target: root.deepLinkRouter
        ignoreUnknownSignals: true
        function onOpenViewRequested(view) { root.bringToFront(); mainView.openView(view) }
        function onOpenTaskRequested(taskId) { root.bringToFront(); mainView.openTask(taskId) }
        function onQuickAddRequested() { if (capture.item) capture.item.present() }
        function onRejected(reason) { mainView.notify("That TaskNest link can't be opened.", "warning") }
    }

    // The global shortcut: compact window only.
    Connections {
        target: root.globalHotkey
        ignoreUnknownSignals: true
        function onActivated() { if (capture.item) capture.item.present() }
    }

    // Text read from another application after the capture shortcut: shown in
    // its own review window; nothing is stored until the user confirms.
    Connections {
        target: root.selectionCapture
        ignoreUnknownSignals: true
        function onFinished(result) { if (selectionWindow.item) selectionWindow.item.present(result) }
    }
    Loader {
        id: selectionWindow
        active: root.taskModel !== null
        sourceComponent: SelectionCaptureWindow {
            taskModel: root.taskModel
            hotkey: root.captureHotkey
        }
    }

    Loader {
        id: capture
        active: root.taskModel !== null
        sourceComponent: QuickCaptureWindow {
            taskModel: root.taskModel
            hotkey: root.globalHotkey
        }
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
        integrationServer: root.integrationServer
        globalHotkey: root.globalHotkey
        captureHotkey: root.captureHotkey
        anchors.fill: parent
        databaseError: root.databaseError
        databasePath: root.databasePath
        taskModel: root.taskModel
        reminderScheduler: root.reminderScheduler
        onReminderArrived: root.alert(0) // flash the taskbar button until focused
    }
}
