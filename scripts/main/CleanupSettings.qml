import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "Completed tasks" settings: the automatic cleanup policy and the manual
// "clear completed" action. Holds three dialogs (settings, confirm save,
// confirm clear). All data access goes through `taskModel`; this file only
// presents the choices and asks before anything is deleted.
Item {
    id: root

    property var taskModel: null // TaskListModel
    property var widgetController: null // WidgetController (show on launch)
    property var shortcutManager: null // ShortcutManager (Start Menu, taskbar)
    property string shortcutNote: ""
    // A short result for the main window's toast.
    signal notice(string text)

    // For tests and diagnostics.
    readonly property alias settingsDialog: settings
    readonly property alias saveConfirmDialog: saveConfirm
    readonly property alias clearConfirmDialog: clearConfirm

    // Mirror CleanupPolicy::Mode.
    readonly property int modeNever: 0
    readonly property int modeImmediately: 1
    readonly property int modeAfterDays: 2

    readonly property var modeLabels: [
        "Never automatically delete",
        "Delete immediately when completed",
        "Delete after a retention period"
    ]
    // The retention periods offered; a saved value outside this list is added
    // so the dialog always shows what is really in effect.
    property var dayOptions: [1, 7, 14, 30]

    readonly property int selectedMode: modeBox.currentIndex
    readonly property int selectedDays: dayOptions[Math.max(daysBox.currentIndex, 0)]
    readonly property bool changed: taskModel !== null
        && (selectedMode !== taskModel.cleanupMode
            || (selectedMode === modeAfterDays && selectedDays !== taskModel.cleanupDays))
    // Completed tasks that saving the selection would delete right away.
    property int impact: 0
    property int completedTotal: 0
    property string errorText: ""

    // What saving would delete follows the selection, however it was changed.
    onSelectedModeChanged: { if (taskModel && settings.visible) refreshCounts() }
    onSelectedDaysChanged: { if (taskModel && settings.visible) refreshCounts() }

    function open() {
        if (taskModel)
            settings.open()
    }

    function dayLabel(n) { return n === 1 ? "1 day" : n + " days" }

    function loadFromModel() {
        let options = [1, 7, 14, 30]
        const current = taskModel.cleanupDays
        if (current > 0 && options.indexOf(current) < 0) {
            options.push(current)
            options.sort(function (a, b) { return a - b })
        }
        dayOptions = options
        modeBox.currentIndex = taskModel.cleanupMode
        const at = options.indexOf(current)
        daysBox.currentIndex = at >= 0 ? at : options.indexOf(7)
        errorText = ""
        refreshCounts()
    }

    function refreshCounts() {
        completedTotal = Math.max(taskModel.completedCount(), 0)
        // Computed here rather than from `changed`, which may not have been
        // re-evaluated yet when this runs from a selection-changed handler.
        const mode = modeBox.currentIndex
        const days = dayOptions[Math.max(daysBox.currentIndex, 0)]
        const differs = mode !== taskModel.cleanupMode
                        || (mode === modeAfterDays && days !== taskModel.cleanupDays)
        if (differs) {
            const n = taskModel.previewCleanup(mode, days)
            impact = Math.max(n, 0)
        } else {
            impact = 0
        }
    }

    function effectText() {
        switch (selectedMode) {
        case modeImmediately:
            return "A task is permanently deleted the moment you complete it. It never appears "
                   + "in Completed, and Undo can’t bring it back."
        case modeAfterDays:
            return "A completed task is permanently deleted once " + dayLabel(selectedDays)
                   + " have passed since it was completed. This is checked when TaskNest starts, "
                   + "when you complete a task, and when you return to the window."
        default:
            return "Completed tasks are kept until you delete them yourself."
        }
    }

    function impactText() {
        return "Saving will permanently delete " + impact + " completed "
               + (impact === 1 ? "task" : "tasks") + " right away."
    }

    function save() {
        errorText = ""
        const mode = selectedMode
        const days = selectedDays
        const removing = taskModel.previewCleanup(mode, days)
        if (removing < 0)
            return // the error is shown
        if (removing > 0) {
            impact = removing
            saveConfirm.open() // explain the consequence before applying it
            return
        }
        commitSave(mode, days)
    }

    function commitSave(mode, days) {
        errorText = ""
        const before = completedTotal
        if (taskModel.setCleanupPolicy(mode, days)) {
            const removed = Math.max(before - Math.max(taskModel.completedCount(), 0), 0)
            let text = "Cleanup saved: " + taskModel.cleanupDescription + "."
            if (removed > 0)
                text += " " + removed + (removed === 1 ? " task" : " tasks") + " permanently deleted."
            root.notice(text)
            settings.close()
        } else {
            // The policy may have been saved even though the cleanup failed.
            const reason = errorText
            loadFromModel()
            errorText = reason
        }
    }

    Connections {
        target: root.taskModel
        function onErrorOccurred(message) {
            if (settings.visible || saveConfirm.visible || clearConfirm.visible)
                root.errorText = message
        }
    }

    // ---- settings -----------------------------------------------------------
    TNDialog {
        id: settings
        objectName: "cleanupDialog"
        title: "Completed tasks"
        width: Math.min(500, parent ? parent.width - 32 : 500)
        property bool closing: false

        onAboutToShow: {
            closing = false
            root.shortcutNote = ""
            if (root.shortcutManager)
                root.shortcutManager.refresh()
            root.loadFromModel()
        }
        onAboutToHide: closing = true
        onOpened: modeBox.forceActiveFocus()

        contentItem: ColumnLayout {
            spacing: Theme.s3

            TNLabel {
                objectName: "cleanupCurrent"
                Layout.fillWidth: true
                role: "label"
                color: Theme.text
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
                text: root.taskModel ? "Current setting: " + root.taskModel.cleanupDescription : ""
            }

            TNLabel {
                Layout.fillWidth: true
                role: "caption"
                text: "Automatic cleanup"
                font.weight: Font.Medium
            }
            TNComboBox {
                id: modeBox
                objectName: "cleanupModeBox"
                Layout.fillWidth: true
                model: root.modeLabels
                Accessible.name: "Automatic cleanup of completed tasks"
            }

            TNLabel {
                visible: root.selectedMode === root.modeAfterDays
                Layout.fillWidth: true
                role: "caption"
                text: "Keep completed tasks for"
                font.weight: Font.Medium
            }
            TNComboBox {
                id: daysBox
                objectName: "cleanupDaysBox"
                visible: root.selectedMode === root.modeAfterDays
                Layout.fillWidth: true
                model: root.dayOptions.map(function (n) { return root.dayLabel(n) })
                Accessible.name: "Retention period"
            }

            TNLabel {
                objectName: "cleanupEffect"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.textMuted
                text: root.effectText()
            }

            // What saving would do to tasks that are already completed.
            Rectangle {
                objectName: "cleanupImpact"
                Layout.fillWidth: true
                visible: root.changed && root.impact > 0
                implicitHeight: impactRow.implicitHeight + 16
                radius: Theme.radiusMd
                color: Theme.warningSoft
                border.color: Theme.warning
                RowLayout {
                    id: impactRow
                    anchors.fill: parent
                    anchors.margins: 8
                    anchors.leftMargin: Theme.s3
                    spacing: Theme.s3
                    Icon { name: "warning"; size: 18; color: Theme.warning; Layout.alignment: Qt.AlignTop }
                    TNLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: root.impactText()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2
                Icon { name: "trash"; size: 16; color: Theme.textMuted; Layout.alignment: Qt.AlignTop }
                TNLabel {
                    Layout.fillWidth: true
                    role: "caption"
                    wrapMode: Text.Wrap
                    text: "Deletion is permanent: removed tasks can’t be restored, and "
                          + "nothing is kept for Undo."
                }
            }

            TNLabel {
                objectName: "cleanupError"
                Layout.fillWidth: true
                visible: root.errorText !== ""
                wrapMode: Text.Wrap
                color: Theme.danger
                text: root.errorText
                Accessible.role: Accessible.AlertMessage
            }

            // ---- Shortcuts & Startup: compact, low priority ----------------
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

            TNLabel {
                role: "overline"
                text: "Shortcuts & Startup"
            }

            ColumnLayout {
                objectName: "shortcutsStartup"
                Layout.fillWidth: true
                spacing: 0

                component MiniSwitch: Switch {
                    id: sw
                    implicitHeight: 26
                    padding: 0
                    leftPadding: 0
                    hoverEnabled: true
                    Layout.fillWidth: true
                    indicator: Rectangle {
                        x: sw.width - width
                        y: (sw.height - height) / 2
                        width: 30
                        height: 16
                        radius: 8
                        color: sw.checked ? Theme.accent : "transparent"
                        border.width: sw.visualFocus ? 2 : 1.5
                        border.color: sw.visualFocus ? Theme.focus
                                      : sw.checked ? Theme.accent : Theme.borderStrong
                        Rectangle {
                            x: sw.checked ? parent.width - width - 3 : 3
                            y: 3
                            width: 10
                            height: 10
                            radius: 5
                            color: sw.checked ? Theme.accentText : Theme.textMuted
                        }
                    }
                    contentItem: Text {
                        text: sw.text
                        verticalAlignment: Text.AlignVCenter
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontLabel
                        elide: Text.ElideRight
                    }
                }

                MiniSwitch {
                    objectName: "startMenuSwitch"
                    text: "Start Menu shortcut"
                    enabled: root.shortcutManager !== null && root.shortcutManager.supported
                    checked: root.shortcutManager ? root.shortcutManager.startMenuShortcut : false
                    onToggled: {
                        root.shortcutNote = ""
                        if (!root.shortcutManager.setStartMenuShortcut(checked))
                            root.shortcutNote = root.shortcutManager.lastError()
                        checked = Qt.binding(function () {
                            return root.shortcutManager.startMenuShortcut })
                    }
                }
                MiniSwitch {
                    objectName: "taskbarPinSwitch"
                    text: "Taskbar pin"
                    enabled: root.shortcutManager !== null && root.shortcutManager.supported
                    checked: root.shortcutManager ? root.shortcutManager.taskbarPinned : false
                    onToggled: {
                        // Windows has no silent pin: guide the user instead.
                        const wasOn = root.shortcutManager.taskbarPinned
                        root.shortcutNote = wasOn
                            ? "Unpin from the taskbar with right-click → Unpin."
                            : root.shortcutManager.requestTaskbarPin()
                                || root.shortcutManager.lastError()
                        checked = Qt.binding(function () {
                            return root.shortcutManager.taskbarPinned })
                    }
                }
                MiniSwitch {
                    objectName: "showOnLaunchSwitch"
                    text: "Show floating window on launch"
                    enabled: root.widgetController !== null
                    checked: root.widgetController ? root.widgetController.showOnLaunch : false
                    onToggled: {
                        root.widgetController.showOnLaunch = checked
                        checked = Qt.binding(function () {
                            return root.widgetController.showOnLaunch })
                    }
                }

                TNLabel {
                    objectName: "shortcutNote"
                    Layout.fillWidth: true
                    visible: root.shortcutNote !== ""
                    role: "caption"
                    wrapMode: Text.Wrap
                    text: root.shortcutNote
                }
            }
        }

        footer: Item {
            implicitHeight: 64

            TNButton {
                objectName: "clearCompletedButton"
                anchors.left: parent.left
                anchors.leftMargin: Theme.s5
                anchors.verticalCenter: parent.verticalCenter
                variant: "ghost"
                iconName: "trash"
                text: "Clear completed…"
                enabled: root.completedTotal > 0
                ToolTip.visible: hovered
                ToolTip.text: root.completedTotal > 0
                              ? "Permanently delete all " + root.completedTotal + " completed tasks"
                              : "There are no completed tasks"
                onClicked: {
                    root.errorText = ""
                    root.completedTotal = Math.max(root.taskModel.completedCount(), 0)
                    if (root.completedTotal > 0)
                        clearConfirm.open()
                }
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: Theme.s5
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s2

                TNButton {
                    objectName: "cleanupClose"
                    variant: "ghost"
                    text: "Close"
                    onClicked: settings.close()
                }
                TNButton {
                    objectName: "cleanupSave"
                    variant: "primary"
                    text: "Save"
                    enabled: root.changed
                    onClicked: {
                        if (settings.closing || !settings.visible)
                            return
                        root.save()
                    }
                }
            }
        }
    }

    // ---- confirm: saving a policy that deletes existing tasks ---------------
    TNDialog {
        id: saveConfirm
        objectName: "cleanupSaveConfirm"
        title: "Delete completed tasks now?"
        width: Math.min(440, parent ? parent.width - 32 : 440)
        property bool closing: false

        onAboutToShow: closing = false
        onAboutToHide: closing = true
        onOpened: saveConfirmCancel.forceActiveFocus() // the safe choice

        contentItem: TNLabel {
            wrapMode: Text.Wrap
            color: Theme.textMuted
            text: "Saving “" + root.modeLabels[root.selectedMode]
                  + (root.selectedMode === root.modeAfterDays ? " (" + root.dayLabel(root.selectedDays) + ")" : "")
                  + "” will permanently delete " + root.impact + " completed "
                  + (root.impact === 1 ? "task" : "tasks")
                  + " that already match it. This can’t be undone. Active tasks are not affected."
        }

        footer: Item {
            implicitHeight: 64
            Row {
                anchors.right: parent.right
                anchors.rightMargin: Theme.s5
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s2

                TNButton {
                    id: saveConfirmCancel
                    objectName: "cleanupSaveCancel"
                    variant: "ghost"
                    text: "Cancel"
                    onClicked: saveConfirm.close()
                }
                TNButton {
                    objectName: "cleanupSaveConfirmButton"
                    variant: "danger"
                    iconName: "trash"
                    text: "Save and delete " + root.impact
                    onClicked: {
                        if (saveConfirm.closing || !saveConfirm.visible)
                            return
                        saveConfirm.closing = true
                        saveConfirm.close()
                        root.commitSave(root.selectedMode, root.selectedDays)
                    }
                }
            }
        }
    }

    // ---- confirm: clear every completed task --------------------------------
    TNDialog {
        id: clearConfirm
        objectName: "clearCompletedConfirm"
        title: "Clear completed tasks?"
        width: Math.min(440, parent ? parent.width - 32 : 440)
        property bool closing: false

        onAboutToShow: closing = false
        onAboutToHide: closing = true
        onOpened: clearCancel.forceActiveFocus() // the safe choice

        contentItem: TNLabel {
            wrapMode: Text.Wrap
            color: Theme.textMuted
            text: "All " + root.completedTotal + " completed "
                  + (root.completedTotal === 1 ? "task" : "tasks")
                  + " will be permanently deleted. This can’t be undone. "
                  + "Active tasks and their reminders are not affected."
        }

        footer: Item {
            implicitHeight: 64
            Row {
                anchors.right: parent.right
                anchors.rightMargin: Theme.s5
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s2

                TNButton {
                    id: clearCancel
                    objectName: "clearCancel"
                    variant: "ghost"
                    text: "Cancel"
                    onClicked: clearConfirm.close()
                }
                TNButton {
                    objectName: "clearConfirmButton"
                    variant: "danger"
                    iconName: "trash"
                    text: "Delete " + root.completedTotal
                    onClicked: {
                        // A second activation while the dialog closes must not run again.
                        if (clearConfirm.closing || !clearConfirm.visible)
                            return
                        clearConfirm.closing = true
                        root.errorText = ""
                        const removed = root.taskModel.clearCompleted()
                        clearConfirm.close()
                        if (removed >= 0) {
                            root.completedTotal = Math.max(root.taskModel.completedCount(), 0)
                            root.impact = 0
                            root.notice(removed + (removed === 1 ? " completed task" : " completed tasks")
                                        + " permanently deleted.")
                        }
                    }
                }
            }
        }
    }
}
