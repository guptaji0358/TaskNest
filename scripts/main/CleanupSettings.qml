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
    property var integrationServer: null // IntegrationServer (local API)
    property var globalHotkey: null // GlobalHotkey (system-wide Quick Add)
    property var captureHotkey: null // GlobalHotkey (capture selection from other apps)
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
    // Page shown in the dialog: 0 General, 1 Shortcuts & integrations, 2 Completed tasks.
    property int tab: 0

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
        target: root.integrationServer
        ignoreUnknownSignals: true
        function onErrorOccurred(message) { root.shortcutNote = message }
    }

    Connections {
        target: root.taskModel
        function onErrorOccurred(message) {
            if (settings.visible || saveConfirm.visible || clearConfirm.visible)
                root.errorText = message
        }
    }

    // ---- building blocks for the settings pages ---------------------------
    // A rounded group of related rows.
    component Card: Rectangle {
        default property alias content: cardColumn.data
        Layout.fillWidth: true
        implicitHeight: cardColumn.implicitHeight + 2
        radius: Theme.radiusMd
        color: Theme.layerLow
        border.width: 1
        border.color: Theme.border
        ColumnLayout {
            id: cardColumn
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0
        }
    }

    // Hairline between rows of a Card.
    component Divider: Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s4
        Layout.rightMargin: Theme.s4
        implicitHeight: 1
        color: Theme.border
    }

    // Page title with a one-line explanation.
    component PageHeader: ColumnLayout {
        property string title: ""
        property string subtitle: ""
        Layout.fillWidth: true
        spacing: 2
        TNLabel { role: "title"; text: parent.title; Layout.fillWidth: true }
        TNLabel {
            role: "caption"
            text: parent.subtitle
            visible: Window.window !== null && Window.window.height > 620
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            color: Theme.textFaint
        }
    }

    // Small caption above a Card.
    component GroupLabel: TNLabel {
        role: "overline"
        Layout.fillWidth: true
        Layout.topMargin: 0
    }

    // One setting: title, a sentence of explanation, a switch on the right.
    // The whole row toggles, so the click target is large.
    component SettingSwitch: Switch {
        id: sw
        property string detail: ""
        Layout.fillWidth: true
        topPadding: 7
        bottomPadding: 7
        leftPadding: Theme.s4
        rightPadding: Theme.s4 + 38 + Theme.s4
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: text
        Accessible.description: detail

        background: Rectangle {
            radius: Theme.radiusMd
            color: sw.hovered && sw.enabled ? Theme.layerLow : "transparent"
            border.width: sw.visualFocus ? 2 : 0
            border.color: Theme.focus
        }
        indicator: Rectangle {
            x: sw.width - width - Theme.s4
            y: (sw.height - height) / 2
            width: 38
            height: 22
            radius: 11
            opacity: sw.enabled ? 1 : 0.45
            color: sw.checked ? Theme.accent : Theme.surfaceRaised
            border.width: 1.5
            border.color: sw.checked ? Theme.accent : Theme.borderStrong
            Rectangle {
                x: sw.checked ? parent.width - width - 3 : 3
                y: 3
                width: 16
                height: 16
                radius: 8
                color: sw.checked ? Theme.accentText : Theme.textMuted
                Behavior on x { NumberAnimation { duration: Theme.durationFast; easing.type: Theme.easeOut } }
            }
        }
        contentItem: ColumnLayout {
            spacing: 2
            TNLabel {
                Layout.fillWidth: true
                text: sw.text
                color: sw.enabled ? Theme.text : Theme.textFaint
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            TNLabel {
                Layout.fillWidth: true
                visible: sw.detail !== ""
                role: "caption"
                text: sw.detail
                wrapMode: Text.Wrap
                color: Theme.textFaint
            }
        }
    }

    // One page of the settings navigation.
    component NavTab: Button {
        id: nav
        required property int index
        required property string label
        required property string glyph
        property bool compact: false
        property bool current: false

        Layout.fillWidth: true
        implicitHeight: 40
        leftPadding: Theme.s3
        rightPadding: Theme.s3
        topPadding: 0
        bottomPadding: 0
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        text: label
        Accessible.name: label + " settings"
        Accessible.role: Accessible.PageTab
        signal chosen()
        onClicked: chosen()
        Keys.onReturnPressed: chosen()
        Keys.onEnterPressed: chosen()

        background: Rectangle {
            radius: Theme.radiusSm
            color: nav.current ? Theme.accentSoft
                   : nav.down ? Theme.layerHigh
                   : nav.hovered ? Theme.layerMid : "transparent"
            border.width: nav.visualFocus ? 2 : 0
            border.color: Theme.focus
            Rectangle {
                visible: nav.current
                width: 3
                height: 18
                radius: 2
                color: Theme.accent
                anchors.left: parent.left
                anchors.leftMargin: 2
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        contentItem: Row {
            spacing: Theme.s3
            Icon {
                name: nav.glyph
                size: 18
                color: nav.current ? Theme.accent : Theme.textMuted
                anchors.verticalCenter: parent.verticalCenter
            }
            TNLabel {
                visible: !nav.compact
                text: nav.label
                color: nav.current ? Theme.text : Theme.textMuted
                font.weight: nav.current ? Font.DemiBold : Font.Medium
                elide: Text.ElideRight
                width: Math.min(implicitWidth, nav.availableWidth - 18 - Theme.s3)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    // A status line and a drop-down for a global shortcut.
    component ShortcutPicker: RowLayout {
        id: picker
        property var hotkey: null
        property string statusName: ""
        property string choiceName: ""
        property string activeNote: ""
        property string accessibleName: ""
        Layout.fillWidth: true
        Layout.leftMargin: Theme.s4
        Layout.rightMargin: Theme.s4
        Layout.bottomMargin: Theme.s3
        visible: hotkey !== null && hotkey.enabled
        spacing: Theme.s3
        TNLabel {
            Layout.fillWidth: true
            role: "caption"
            objectName: picker.statusName
            wrapMode: Text.Wrap
            color: picker.hotkey && picker.hotkey.registered ? Theme.textMuted : Theme.warning
            text: !picker.hotkey ? ""
                  : picker.hotkey.registered
                    ? "Active: " + picker.hotkey.shortcut + picker.activeNote
                    : picker.hotkey.lastError
        }
        TNComboBox {
            objectName: picker.choiceName
            Layout.preferredWidth: 170
            implicitHeight: 30
            model: picker.hotkey ? picker.hotkey.choices : []
            currentIndex: picker.hotkey
                          ? Math.max(0, picker.hotkey.choices.indexOf(picker.hotkey.shortcut)) : 0
            Accessible.name: picker.accessibleName
            onActivated: picker.hotkey.shortcut = currentText
        }
    }

    // ---- settings -----------------------------------------------------------
    TNDialog {
        id: settings
        objectName: "cleanupDialog"
        title: "Settings"
        width: Math.min(780, parent ? parent.width - 32 : 780)
        property bool closing: false
        readonly property bool compactNav: width < 620

        onAboutToShow: {
            closing = false
            root.tab = 0
            root.shortcutNote = ""
            if (root.shortcutManager)
                root.shortcutManager.refresh()
            root.loadFromModel()
        }
        onAboutToHide: closing = true
        onOpened: tabRepeater.itemAt(0).forceActiveFocus()

        // A fixed height, so switching pages never resizes the dialog.
        contentItem: Item {
            implicitWidth: 700
            implicitHeight: Math.max(300, Math.min(600, (settings.parent ? settings.parent.height : 640) - 148))

            RowLayout {
                anchors.fill: parent
                spacing: Theme.s4

                // Navigation
                Rectangle {
                    Layout.fillHeight: true
                    Layout.preferredWidth: settings.compactNav ? 56 : 200
                    radius: Theme.radiusMd
                    color: Theme.sidebar
                    border.width: 1
                    border.color: Theme.border

                    ColumnLayout {
                        objectName: "settingsTabs"
                        anchors.fill: parent
                        anchors.margins: Theme.s2
                        spacing: Theme.s1
                        Repeater {
                            id: tabRepeater
                            model: [
                                { label: "General", glyph: "settings" },
                                { label: "Shortcuts & integrations", glyph: "keyboard" },
                                { label: "Completed tasks", glyph: "completed" }
                            ]
                            NavTab {
                                required property var modelData
                                objectName: "settingsTab" + index
                                label: modelData.label
                                glyph: modelData.glyph
                                compact: settings.compactNav
                                current: root.tab === index
                                onChosen: root.tab = index
                                ToolTip.visible: compact && hovered
                                ToolTip.text: modelData.label
                            }
                        }
                        Item { Layout.fillHeight: true }
                    }
                }

                // Page
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: Theme.s2

                    Flickable {
                        id: flick
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: width
                        contentHeight: pages.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: TNScrollBar {}

                        Connections {
                            target: root
                            function onTabChanged() { flick.contentY = 0 }
                        }

                        // Keyboard focus never lands on a row scrolled out of view.
                        function reveal(item) {
                            let p = item
                            while (p && p !== pages)
                                p = p.parent
                            if (!p)
                                return
                            const y = item.mapToItem(pages, 0, 0).y
                            const top = y - Theme.s2
                            const bottom = y + item.height + Theme.s2
                            if (top < contentY)
                                contentY = Math.max(0, top)
                            else if (bottom > contentY + height)
                                contentY = Math.min(bottom - height, Math.max(0, contentHeight - height))
                        }
                        Connections {
                            target: flick.Window.window
                            ignoreUnknownSignals: true
                            function onActiveFocusItemChanged() {
                                if (settings.visible && flick.Window.window.activeFocusItem)
                                    flick.reveal(flick.Window.window.activeFocusItem)
                            }
                        }

                        ColumnLayout {
                            id: pages
                            width: flick.width - Theme.s3
                            spacing: Theme.s3

                            // ---- General ------------------------------------
                            ColumnLayout {
                                objectName: "generalTab"
                                visible: root.tab === 0
                                Layout.fillWidth: true
                                spacing: Theme.s3

                                PageHeader {
                                    title: "General"
                                    subtitle: "How TaskNest appears on this computer. Nothing here starts "
                                              + "with Windows unless you turn that on."
                                }

                                GroupLabel { text: "Windows" }
                                Card {
                                    SettingSwitch {
                                        objectName: "startMenuSwitch"
                                        text: "Start Menu shortcut"
                                        detail: "Adds TaskNest to the Start menu so you can open it from there."
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
                                    Divider {}
                                    SettingSwitch {
                                        objectName: "startWithWindowsSwitch"
                                        text: "Start with Windows"
                                        detail: "Starts TaskNest hidden in the notification area when you sign in, "
                                                + "so reminders work after a restart. Needs \"Keep running in "
                                                + "the background\" (Appearance)."
                                        enabled: root.shortcutManager !== null && root.shortcutManager.supported
                                                 && root.widgetController !== null
                                                 && (root.widgetController.runInBackground
                                                     || root.shortcutManager.startWithWindows)
                                        checked: root.shortcutManager ? root.shortcutManager.startWithWindows : false
                                        onToggled: {
                                            root.shortcutNote = ""
                                            if (!root.shortcutManager.setStartWithWindows(checked))
                                                root.shortcutNote = root.shortcutManager.lastError()
                                            checked = Qt.binding(function () {
                                                return root.shortcutManager.startWithWindows })
                                        }
                                    }
                                    Divider {}
                                    SettingSwitch {
                                        objectName: "taskbarPinSwitch"
                                        text: "Taskbar pin"
                                        detail: "Windows has no silent pin, so TaskNest shows you where to pin it."
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
                                }

                                GroupLabel { text: "Appearance" }
                                Card {
                                    SettingSwitch {
                                        objectName: "reduceMotionSwitch"
                                        text: "Reduce motion"
                                        detail: "Turns off slides and fades. Everything changes instantly."
                                        enabled: root.widgetController !== null
                                        checked: root.widgetController ? root.widgetController.reduceMotion : false
                                        onToggled: {
                                            root.widgetController.reduceMotion = checked
                                            checked = Qt.binding(function () {
                                                return root.widgetController.reduceMotion })
                                        }
                                    }
                                    Divider {}
                                    SettingSwitch {
                                        objectName: "showOnLaunchSwitch"
                                        text: "Show floating window on launch"
                                        detail: "Opens the small always-on-top task window when TaskNest starts."
                                        enabled: root.widgetController !== null
                                        checked: root.widgetController ? root.widgetController.showOnLaunch : false
                                        onToggled: {
                                            root.widgetController.showOnLaunch = checked
                                            checked = Qt.binding(function () {
                                                return root.widgetController.showOnLaunch })
                                        }
                                    }
                                    Divider {}
                                    SettingSwitch {
                                        objectName: "runInBackgroundSwitch"
                                        text: "Keep running in the background"
                                        detail: "Closing the window hides TaskNest to the notification area so "
                                                + "reminders still arrive. Quit from its tray icon. Off: "
                                                + "closing quits."
                                        enabled: root.widgetController !== null
                                        checked: root.widgetController ? root.widgetController.runInBackground : false
                                        onToggled: {
                                            root.widgetController.runInBackground = checked
                                            checked = Qt.binding(function () {
                                                return root.widgetController.runInBackground })
                                        }
                                    }
                                }
                            }

                            // ---- Shortcuts & integrations -------------------
                            ColumnLayout {
                                objectName: "shortcutsTab"
                                visible: root.tab === 1
                                Layout.fillWidth: true
                                spacing: Theme.s3

                                PageHeader {
                                    title: "Shortcuts & integrations"
                                    subtitle: "Everything is off until you turn it on, and stays on this "
                                              + "computer."
                                }

                                GroupLabel { text: "Global shortcuts" }
                                Card {
                                    SettingSwitch {
                                        objectName: "hotkeySwitch"
                                        text: "Global Quick Add shortcut"
                                        detail: "Opens a small window to add a task from any app."
                                        enabled: root.globalHotkey !== null
                                        checked: root.globalHotkey ? root.globalHotkey.enabled : false
                                        onToggled: {
                                            root.shortcutNote = ""
                                            root.globalHotkey.enabled = checked
                                            checked = Qt.binding(function () { return root.globalHotkey.enabled })
                                        }
                                    }
                                    ShortcutPicker {
                                        hotkey: root.globalHotkey
                                        statusName: "hotkeyStatus"
                                        choiceName: "hotkeyChoice"
                                        accessibleName: "Quick Add shortcut"
                                    }
                                    Divider {}
                                    SettingSwitch {
                                        objectName: "captureSwitch"
                                        text: "Capture selection from other apps"
                                        detail: "Turns the text you selected elsewhere into a task, after you review it."
                                        enabled: root.captureHotkey !== null
                                        checked: root.captureHotkey ? root.captureHotkey.enabled : false
                                        onToggled: {
                                            root.shortcutNote = ""
                                            root.captureHotkey.enabled = checked
                                            checked = Qt.binding(function () { return root.captureHotkey.enabled })
                                        }
                                    }
                                    ShortcutPicker {
                                        hotkey: root.captureHotkey
                                        statusName: "captureStatus"
                                        choiceName: "captureChoice"
                                        accessibleName: "Capture selection shortcut"
                                        activeNote: ". Select text in another app, press it, review, then Capture."
                                    }
                                    TNLabel {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: Theme.s4
                                        Layout.rightMargin: Theme.s4
                                        Layout.bottomMargin: Theme.s3
                                        visible: root.captureHotkey !== null && root.captureHotkey.enabled
                                        role: "caption"
                                        color: Theme.textFaint
                                        wrapMode: Text.Wrap
                                        text: "Works only with apps that share their selection through Windows UI "
                                              + "Automation. Others open an empty window to paste into. Password "
                                              + "fields are never read; the clipboard is not touched."
                                    }
                                }

                                GroupLabel { text: "Connections" }
                                Card {
                                    SettingSwitch {
                                        objectName: "linksSwitch"
                                        text: "Open tasknest:// links"
                                        detail: "Lets links and other programs open a task or view in TaskNest."
                                        enabled: root.shortcutManager !== null && root.shortcutManager.supported
                                        checked: root.shortcutManager ? root.shortcutManager.linksRegistered : false
                                        onToggled: {
                                            root.shortcutNote = ""
                                            if (!root.shortcutManager.setLinksRegistered(checked))
                                                root.shortcutNote = root.shortcutManager.lastError()
                                            checked = Qt.binding(function () {
                                                return root.shortcutManager.linksRegistered })
                                        }
                                    }
                                    Divider {}
                                    SettingSwitch {
                                        objectName: "browserHostSwitch"
                                        text: "Browser capture (Chrome / Edge)"
                                        detail: "Lets the TaskNest browser extension save the current page as a task."
                                        enabled: root.shortcutManager !== null && root.shortcutManager.supported
                                        checked: root.shortcutManager ? root.shortcutManager.browserHostRegistered : false
                                        onToggled: {
                                            root.shortcutNote = ""
                                            if (!root.shortcutManager.setBrowserHostRegistered(checked))
                                                root.shortcutNote = root.shortcutManager.lastError()
                                            else if (checked && root.integrationServer && !root.integrationServer.enabled)
                                                root.shortcutNote = "Also turn on “Local integration API” below; "
                                                                    + "browser capture needs it."
                                            checked = Qt.binding(function () {
                                                return root.shortcutManager.browserHostRegistered })
                                        }
                                    }
                                    Divider {}
                                    SettingSwitch {
                                        objectName: "integrationSwitch"
                                        text: "Local integration API"
                                        detail: "A private channel on this computer for tools you trust. No network."
                                        enabled: root.integrationServer !== null
                                        checked: root.integrationServer ? root.integrationServer.listening : false
                                        onToggled: {
                                            root.shortcutNote = ""
                                            root.integrationServer.enabled = checked
                                            checked = Qt.binding(function () {
                                                return root.integrationServer.listening })
                                        }
                                    }
                                }
                            }

                            // ---- Completed tasks ----------------------------
                            ColumnLayout {
                                objectName: "completedTab"
                                visible: root.tab === 2
                                Layout.fillWidth: true
                                spacing: Theme.s3

                                PageHeader {
                                    title: "Completed tasks"
                                    subtitle: "Completed tasks are kept forever unless you choose otherwise."
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: currentRow.implicitHeight + Theme.s3 * 2
                                    radius: Theme.radiusMd
                                    color: Theme.accentSoft
                                    border.width: 1
                                    border.color: Theme.accentLine
                                    RowLayout {
                                        id: currentRow
                                        anchors.fill: parent
                                        anchors.margins: Theme.s3
                                        anchors.leftMargin: Theme.s4
                                        spacing: Theme.s3
                                        Icon { name: "check"; size: 18; color: Theme.accent }
                                        TNLabel {
                                            objectName: "cleanupCurrent"
                                            Layout.fillWidth: true
                                            role: "label"
                                            color: Theme.text
                                            font.weight: Font.DemiBold
                                            wrapMode: Text.Wrap
                                            text: root.taskModel ? "Current setting: " + root.taskModel.cleanupDescription : ""
                                        }
                                    }
                                }

                                GroupLabel { text: "Automatic cleanup" }
                                Card {
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.margins: Theme.s4
                                        spacing: Theme.s2

                                        TNLabel {
                                            Layout.fillWidth: true
                                            role: "caption"
                                            text: "When a task is completed"
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
                                            Layout.topMargin: Theme.s1
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
                                            Layout.topMargin: Theme.s1
                                            wrapMode: Text.Wrap
                                            role: "caption"
                                            color: Theme.textMuted
                                            text: root.effectText()
                                        }
                                    }
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
                            }
                        }
                    }

                    // Messages stay visible however far the page is scrolled.
                    TNLabel {
                        objectName: "cleanupError"
                        Layout.fillWidth: true
                        visible: root.errorText !== "" && root.tab === 2
                        wrapMode: Text.Wrap
                        color: Theme.danger
                        text: root.errorText
                        Accessible.role: Accessible.AlertMessage
                    }
                    TNLabel {
                        objectName: "shortcutNote"
                        Layout.fillWidth: true
                        visible: root.shortcutNote !== "" && root.tab !== 2
                        role: "caption"
                        wrapMode: Text.Wrap
                        text: root.shortcutNote
                    }
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
                visible: root.tab === 2
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
                    visible: root.tab === 2
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
