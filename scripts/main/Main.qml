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
    property bool startHidden: false   // started by Windows at sign-in: begin in the tray

    width: 1040
    height: 700
    minimumWidth: 800
    minimumHeight: 520
    visible: !startHidden
    title: "TaskNest"
    color: Theme.bg

    Component.onCompleted: Theme.reducedMotion = reducedMotion

    // With "Keep running in the background" (and a tray icon to get back in)
    // closing only hides the window. Otherwise the application ends
    // (preferences are written first), so nothing is left running.
    readonly property bool keepRunning: widgetController !== null
                                        && widgetController.runInBackground
                                        && trayLoader.item !== null
                                        && trayLoader.item.available

    function quitApplication() {
        if (widgetController)
            widgetController.prepareForShutdown()
        Qt.quit()
    }

    onClosing: (close) => {
        if (keepRunning) {
            close.accepted = false
            hide()
        } else {
            quitApplication()
        }
    }

    Loader {
        id: trayLoader
        active: root.widgetController !== null && root.widgetController.runInBackground
        sourceComponent: TrayIcon {
            windowHidden: !root.visible || root.visibility === Window.Minimized
            onOpenRequested: root.bringToFront()
            onQuitRequested: root.quitApplication()
        }
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

    Connections {
        target: root.reminderScheduler
        ignoreUnknownSignals: true
        function onReminderFired(taskId, title, reminderAt) {
            if (trayLoader.item)
                trayLoader.item.announce(title)
        }
    }

    // A second launch while this window is hidden to the tray.
    Connections {
        target: root.deepLinkRouter
        ignoreUnknownSignals: true
        function onShowWindowRequested() { root.bringToFront() }
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

    // The system title bar is replaced by TitleBar; WindowChrome keeps the
    // rest of the native behavior (snap, resize borders, shadow). When it
    // cannot attach (any other platform), the normal frame stays.
    WindowChrome {
        id: chrome
        window: root
        maximizeButtonRect: titleBar.maximizeRect
    }

    TitleBar {
        id: titleBar
        objectName: "titleBar"
        visible: chrome.active
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? Theme.titleBarHeight : 0
        window: root
        maximized: chrome.maximized
        maximizeHovered: chrome.maximizeHovered
        maximizePressed: chrome.maximizePressed
        railMode: mainView.railMode
        context: mainView.databaseOk ? mainView.currentNav.label : ""
        widgetAvailable: root.widgetController !== null
        widgetActive: root.widgetController ? root.widgetController.widgetVisible : false
        onToggleSidebarRequested: mainView.forceRail = !mainView.forceRail
        onWidgetToggleRequested: mainView.toggleWidget()
        onSettingsRequested: mainView.openSettings()
        onMinimizeRequested: root.showMinimized()
        onMaximizeRequested: chrome.toggleMaximized()
        onCloseRequested: root.close()
    }

    MainView {
        id: mainView
        titleSearchHost: titleBar.visible && mainView.databaseOk ? titleBar.searchSlot : null
        widgetController: root.widgetController
        shortcutManager: root.shortcutManager
        integrationServer: root.integrationServer
        globalHotkey: root.globalHotkey
        captureHotkey: root.captureHotkey
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        databaseError: root.databaseError
        databasePath: root.databasePath
        taskModel: root.taskModel
        reminderScheduler: root.reminderScheduler
        onReminderArrived: root.alert(0) // flash the taskbar button until focused
    }
}
