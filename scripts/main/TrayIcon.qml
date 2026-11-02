import QtQuick
import Qt.labs.platform as Platform

// Notification-area icon for "Keep running in the background". Created only
// while that preference is on. It owns no timers: reminders still come from
// ReminderScheduler, this only shows them when the main window is hidden.
Platform.SystemTrayIcon {
    id: root

    property bool windowHidden: false

    signal openRequested()
    signal quitRequested()

    visible: true
    icon.source: "qrc:/icons/tasknest-256.png"
    tooltip: "TaskNest"

    menu: Platform.Menu {
        Platform.MenuItem {
            text: "Open TaskNest"
            onTriggered: root.openRequested()
        }
        Platform.MenuSeparator {}
        Platform.MenuItem {
            text: "Quit TaskNest"
            onTriggered: root.quitRequested()
        }
    }

    onActivated: (reason) => {
        if (reason === Platform.SystemTrayIcon.Trigger
                || reason === Platform.SystemTrayIcon.DoubleClick)
            root.openRequested()
    }
    onMessageClicked: root.openRequested()

    // Shown only while the main window is hidden; otherwise its banner is enough.
    function announce(title) {
        if (root.windowHidden && root.supportsMessages)
            root.showMessage("Reminder", title, Platform.SystemTrayIcon.Information, 15000)
    }
}
