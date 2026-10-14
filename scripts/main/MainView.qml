import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The task interface. All data access goes through `taskModel`
// (TaskListModel); nothing here touches SQL. Styling comes from Theme and
// the TN* components.
Item {
    id: root

    required property string databaseError
    required property string databasePath
    required property var taskModel // null when the database is unavailable
    property var reminderScheduler: null // ReminderScheduler, optional

    // Emitted when a reminder is announced (the window can flash the taskbar).
    signal reminderArrived()

    readonly property bool databaseOk: databaseError.length === 0 && taskModel !== null

    // Mirror TaskListModel::Scope and TaskListModel::CompletionFilter.
    readonly property int scopeEverything: 0
    readonly property int scopeToday: 1
    readonly property int scopeUpcoming: 2
    readonly property int scopeList: 3
    readonly property int filterAll: 0
    readonly property int filterActive: 1
    readonly property int filterCompleted: 2

    // For tests and diagnostics.
    readonly property alias editorDialog: editor
    readonly property alias deleteConfirmDialog: deleteDialog
    readonly property alias cleanupSettings: cleanup

    property string navKey: "all"
    property string errorText: ""   // last model error, shown in the banner
    property string addMessage: ""  // validation message under the add field
    property bool forceRail: false  // user collapsed the sidebar
    property bool quickDetails: false   // Quick Add details panel open
    property double lastAddAt: 0        // ms timestamp of the last successful add
    property bool undoToastActive: false // the toast currently offers "Undo delete"
    property int undoWindowMs: 8000     // how long a deletion can be undone
    // Narrow windows always get the icon rail so navigation stays reachable.
    readonly property bool railMode: forceRail || width < Theme.railBreakpoint

    // Controls inherit this palette, so anything not styled explicitly
    // (selection, tooltips, popups) still matches the theme.
    palette.window: Theme.bg
    palette.windowText: Theme.text
    palette.base: Theme.surfaceRaised
    palette.text: Theme.text
    palette.button: Theme.surfaceRaised
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentText
    palette.toolTipBase: Theme.elevated
    palette.toolTipText: Theme.text
    palette.mid: Theme.borderStrong
    palette.dark: Theme.border

    // Sidebar entries: fixed views, then the lists found in the data.
    readonly property var navItems: {
        const items = [
            { key: "all", label: "All Tasks", icon: "all", scope: scopeEverything, list: "", completion: filterAll },
            { key: "today", label: "Today", icon: "today", scope: scopeToday, list: "", completion: filterAll },
            { key: "upcoming", label: "Upcoming", icon: "upcoming", scope: scopeUpcoming, list: "", completion: filterActive },
            { key: "completed", label: "Completed", icon: "completed", scope: scopeEverything, list: "", completion: filterCompleted },
            { key: "list:Inbox", label: "Inbox", icon: "inbox", scope: scopeList, list: "Inbox", completion: filterAll }
        ]
        const names = taskModel ? taskModel.listNames : []
        let custom = []
        for (let i = 0; i < names.length; ++i) {
            if (names[i] !== "Inbox")
                custom.push({ key: "list:" + names[i], label: names[i], icon: "list",
                              scope: scopeList, list: names[i], completion: filterAll })
        }
        if (custom.length > 0)
            items.push({ key: "header", label: "Lists", header: true })
        return items.concat(custom)
    }

    readonly property var currentNav: {
        for (let i = 0; i < navItems.length; ++i) {
            if (navItems[i].key === navKey)
                return navItems[i]
        }
        return navItems[0]
    }
    // Upcoming and Completed define their own completion state.
    readonly property bool completionFilterLocked:
        navKey === "upcoming" || navKey === "completed"
    readonly property bool canAdd:
        databaseOk && navKey !== "completed" && taskModel.completionFilter !== filterCompleted

    // A hint about the add field makes no sense once it cannot be used.
    onCanAddChanged: { if (!canAdd) addMessage = "" }

    function selectNav(item) {
        errorText = ""
        addMessage = ""
        if (taskModel.setView(item.scope, item.list, item.completion))
            navKey = item.key
    }

    // Focuses the add field, leaving a view that cannot take new tasks first.
    function focusAdd() {
        if (!databaseOk)
            return
        if (!canAdd)
            selectNav(navItems[0])
        addField.forceActiveFocus()
    }

    // Moves keyboard focus to a task row (or back to search above the first).
    function focusRow(index) {
        if (index < 0) {
            searchField.forceActiveFocus()
            return
        }
        if (index >= taskList.count)
            return
        taskList.currentIndex = index
        taskList.positionViewAtIndex(index, ListView.Contain)
        const item = taskList.itemAtIndex(index)
        if (item) {
            item.forceActiveFocus()
        } else {
            // The delegate is created on the next frame (list just changed).
            Qt.callLater(function () {
                const created = taskList.itemAtIndex(index)
                if (created)
                    created.forceActiveFocus()
            })
        }
    }

    function focusList() {
        if (taskList.count > 0)
            focusRow(Math.max(taskList.currentIndex, 0))
    }

    function addTask() {
        errorText = ""
        const title = addField.text.trim()
        if (title === "") {
            // A second click/Enter right after a successful add is a repeat,
            // not a mistake worth a warning (and it must not add anything).
            if (Date.now() - lastAddAt < 700)
                return
            addMessage = "Enter a task title first."
            addField.forceActiveFocus()
            return
        }
        addMessage = ""

        // Optional details (priority, list, due, reminder, note).
        const details = quickPanel.collect()
        if (details.error) {
            quickDetails = true // show the field that needs fixing
            return
        }

        // Give the new task what the current view needs to show it, unless
        // the user chose something explicitly.
        let due = details.due
        if (isNaN(due.getTime()) && (navKey === "today" || navKey === "upcoming")) {
            due = new Date()
            if (navKey === "upcoming")
                due.setDate(due.getDate() + 1)
            due.setHours(23, 59, 0, 0)
        }
        let list = details.list.trim()
        if (list === "" && currentNav.scope === scopeList)
            list = currentNav.list

        const countBefore = taskModel.count
        if (taskModel.addTask(title, details.notes, list, details.priority, due, details.reminder)) {
            lastAddAt = Date.now()
            addField.clear()
            quickPanel.reset()
            if (searchField.text !== "")
                searchField.clear() // otherwise the new task could be hidden
            if (taskModel.count === countBefore) // filtered out of the current view
                notify("Added \u201C" + title + "\u201D \u2014 it is not shown in this view.", "check")
        } // on failure the banner shows the error and the text is kept
    }

    // A short notice. It replaces (and so retires) a pending "Undo delete" offer.
    function notify(text, icon) {
        discardUndoOffer()
        toast.show(text, "", 5000, icon || "check")
    }

    function discardUndoOffer() {
        if (undoToastActive) {
            undoToastActive = false
            if (taskModel)
                taskModel.discardUndo()
        }
    }

    function offerUndo(title) {
        undoToastActive = true
        toast.show("\u201C" + title + "\u201D deleted", qsTr("Undo"), undoWindowMs, "trash")
    }

    function undoDelete() {
        if (!undoToastActive || !taskModel.canUndo)
            return // already used, expired, or replaced: never restore twice
        errorText = ""
        const title = taskModel.undoTitle
        if (taskModel.undoDelete()) {
            undoToastActive = false
            toast.hide()
            notify("Restored \u201C" + title + "\u201D", "check")
        } // on failure the error banner explains; the offer stays for a retry
    }

    // True while the keyboard is typing into a text field (so Ctrl+Z must
    // stay a text-undo there).
    readonly property bool textInputHasFocus: {
        const w = root.Window.window
        const item = w ? w.activeFocusItem : null
        return item !== null && item !== undefined && item.selectByMouse !== undefined
    }

    function openQuickAdd() {
        if (!databaseOk)
            return
        if (!canAdd)
            selectNav(navItems[0])
        quickDetails = true
        addField.forceActiveFocus()
    }

    function subtitle() {
        switch (navKey) {
        case "all": return "Everything on your plate"
        case "today": return Qt.formatDate(new Date(), "dddd, d MMMM")
        case "upcoming": return "Open tasks due after today"
        case "completed": return "Finished work"
        case "list:Inbox": return "The default home for new tasks"
        default: return "Custom list"
        }
    }

    function emptyTitle() {
        if (searchField.text.trim() !== "")
            return "No matches"
        switch (navKey) {
        case "all": return taskModel.completionFilter === filterAll ? "A clean slate" : "Nothing here"
        case "today": return "Nothing due today"
        case "upcoming": return "Nothing coming up"
        case "completed": return "Nothing finished yet"
        default: return "Nothing here"
        }
    }

    // Lime tick only for "all clear"; searches and filters get a neutral token.
    function emptyAccented() {
        return searchField.text.trim() === "" && navKey === "all"
               && taskModel.completionFilter === filterAll
    }

    function emptyIcon() {
        if (searchField.text.trim() !== "")
            return "search"
        switch (navKey) {
        case "today": return "today"
        case "upcoming": return "upcoming"
        case "completed": return "completed"
        case "all": return taskModel.completionFilter === filterAll ? "check" : "all"
        default: return "list"
        }
    }

    function emptyMessage() {
        const search = searchField.text.trim()
        if (search !== "")
            return "No tasks match “" + search + "”."
        const filter = taskModel.completionFilter
        const qualifier = filter === filterActive ? "active " : filter === filterCompleted ? "completed " : ""
        switch (navKey) {
        case "all":
            return qualifier === "" ? "You have no tasks yet. Add your first task above."
                                    : "No " + qualifier + "tasks."
        case "today":
            return "No " + qualifier + "tasks due today."
        case "upcoming":
            return "No upcoming tasks."
        case "completed":
            return "No completed tasks yet."
        default:
            return "No " + qualifier + "tasks in " + currentNav.label + "."
        }
    }

    // Refresh when the app returns to the foreground so "Today" is current
    // after midnight. No timers or polling.
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive && root.databaseOk)
                root.taskModel.refresh()
        }
    }

    // Reminders that have fired and not been acknowledged yet.
    ListModel { id: reminders }

    function reminderIndex(taskId) {
        for (let i = 0; i < reminders.count; ++i) {
            if (reminders.get(i).taskId === taskId)
                return i
        }
        return -1
    }

    Connections {
        target: root.reminderScheduler
        function onReminderFired(taskId, title, reminderAt) {
            if (root.reminderIndex(taskId) < 0)
                reminders.append({ taskId: taskId, title: title, when: reminderAt })
            root.reminderArrived()
        }
        function onReminderCancelled(taskId) {
            const i = root.reminderIndex(taskId)
            if (i >= 0)
                reminders.remove(i)
        }
        function onErrorOccurred(message) { root.errorText = message }
    }

    // Marks the reminder handled; the banner goes away only if that was stored.
    function dismissReminder(taskId, alsoComplete) {
        if (!root.reminderScheduler.acknowledge(taskId))
            return
        if (alsoComplete)
            root.taskModel.setCompleted(taskId, true)
        const i = reminderIndex(taskId)
        if (i >= 0)
            reminders.remove(i)
    }

    Connections {
        target: root.taskModel
        function onErrorOccurred(message) { root.errorText = message }
        function onCompletionFilterChanged() { filterBox.currentIndex = root.taskModel.completionFilter }
        // The cleanup policy removed completed tasks without being asked each time.
        function onCompletedTasksRemoved(count) {
            root.notify(count + (count === 1 ? " completed task was" : " completed tasks were")
                        + " permanently removed by your cleanup setting.", "trash")
        }
        function onListNamesChanged() {
            // The list being viewed no longer has tasks: fall back to All Tasks.
            if (root.navKey.startsWith("list:") && root.navKey !== "list:Inbox"
                    && root.taskModel.listNames.indexOf(root.navKey.substring(5)) < 0)
                root.selectNav(root.navItems[0])
        }
    }

    Shortcut {
        sequence: "Ctrl+N"
        enabled: root.databaseOk
        onActivated: root.focusAdd()
    }
    Shortcut {
        sequence: "Ctrl+F"
        enabled: root.databaseOk
        onActivated: searchField.forceActiveFocus()
    }
    Shortcut {
        sequence: "Ctrl+Shift+N"
        enabled: root.databaseOk
        onActivated: root.openQuickAdd()
    }
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: root.undoToastActive && !root.textInputHasFocus
        onActivated: root.undoDelete()
    }
    Shortcut {
        sequence: "F1"
        enabled: root.databaseOk
        onActivated: shortcutsDialog.open()
    }
    // Ctrl+1..5: All Tasks, Today, Upcoming, Completed, Inbox
    Shortcut {
        sequence: "Ctrl+1"
        enabled: root.databaseOk
        onActivated: root.selectNav(root.navItems[0])
    }
    Shortcut {
        sequence: "Ctrl+2"
        enabled: root.databaseOk
        onActivated: root.selectNav(root.navItems[1])
    }
    Shortcut {
        sequence: "Ctrl+3"
        enabled: root.databaseOk
        onActivated: root.selectNav(root.navItems[2])
    }
    Shortcut {
        sequence: "Ctrl+4"
        enabled: root.databaseOk
        onActivated: root.selectNav(root.navItems[3])
    }
    Shortcut {
        sequence: "Ctrl+5"
        enabled: root.databaseOk
        onActivated: root.selectNav(root.navItems[4])
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // ---- database unavailable -------------------------------------------
    ColumnLayout {
        visible: !root.databaseOk
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 560)
        spacing: Theme.s4

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 56
            height: 56
            radius: 18
            color: Theme.dangerSoft
            border.color: Theme.dangerLine
            Icon { anchors.centerIn: parent; name: "warning"; size: 26; color: Theme.danger }
        }
        TNLabel {
            Layout.alignment: Qt.AlignHCenter
            role: "heading"
            text: "TaskNest can’t open its database"
        }
        TNLabel {
            objectName: "unavailableMessage"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: Theme.danger
            text: "Database unavailable. Your data has not been modified.\n" + root.databaseError
            Accessible.role: Accessible.AlertMessage
        }
    }

    // ---- main interface -------------------------------------------------
    RowLayout {
        visible: root.databaseOk
        anchors.fill: parent
        spacing: 0

        // Sidebar / rail
        Rectangle {
            id: sidebar
            Layout.preferredWidth: root.railMode ? Theme.railWidth : Theme.sidebarWidth
            Layout.fillHeight: true
            color: Theme.sidebar
            Behavior on Layout.preferredWidth { NumberAnimation { duration: Theme.durationBase; easing.type: Easing.OutCubic } }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: root.railMode ? Theme.s2 + 2 : Theme.s3 + 2
                anchors.topMargin: Theme.s4
                spacing: Theme.s1

                // Wordmark: a lime tile holding a nest, then "Task" + "Nest".
                Item {
                    Layout.fillWidth: true
                    implicitHeight: 38

                    Rectangle {
                        id: logoTile
                        width: 34
                        height: 34
                        radius: 11
                        color: Theme.accent
                        anchors.verticalCenter: parent.verticalCenter
                        x: root.railMode ? (parent.width - width) / 2 : Theme.s2
                        Icon {
                            anchors.centerIn: parent
                            name: "nest"
                            size: 22
                            strokeWidth: 2.2
                            color: Theme.accentText
                        }
                    }
                    TNLabel {
                        visible: !root.railMode
                        anchors.left: logoTile.right
                        anchors.leftMargin: Theme.s3
                        anchors.verticalCenter: parent.verticalCenter
                        textFormat: Text.StyledText
                        text: "Task<font color=\"" + Theme.accent + "\">Nest</font>"
                        font.family: Theme.displayFamily
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                        font.letterSpacing: -0.2
                    }
                }

                Item { implicitHeight: Theme.s2 }

                TNButton {
                    id: newTaskButton
                    objectName: "newTaskButton"
                    Layout.fillWidth: true
                    variant: "tonal"
                    iconName: "plus"
                    text: root.railMode ? "" : "New task"
                    leftPadding: root.railMode ? 0 : Theme.s4
                    rightPadding: root.railMode ? 0 : Theme.s4
                    Accessible.name: "New task (Ctrl+N)"
                    ToolTip.visible: hovered && root.railMode
                    ToolTip.text: "New task (Ctrl+N)"
                    onClicked: root.focusAdd()
                }

                Item { implicitHeight: Theme.s2 }

                ListView {
                    id: navList
                    objectName: "navList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 0
                    boundsBehavior: Flickable.StopAtBounds
                    model: root.navItems
                    ScrollBar.vertical: TNScrollBar {}

                    delegate: Item {
                        id: navEntry
                        required property var modelData
                        width: ListView.view.width
                        height: modelData.header ? (root.railMode ? 14 : 32) : 36

                        TNLabel {
                            visible: navEntry.modelData.header === true && !root.railMode
                            role: "overline"
                            text: navEntry.modelData.label
                            x: Theme.s3 + 2
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: Theme.s2
                        }
                        Rectangle {
                            visible: navEntry.modelData.header === true && root.railMode
                            height: 1
                            color: Theme.border
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: Theme.s3
                            anchors.rightMargin: Theme.s3
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        NavItem {
                            visible: navEntry.modelData.header !== true
                            objectName: "nav_" + navEntry.modelData.key
                            anchors.fill: parent
                            iconName: navEntry.modelData.icon || ""
                            label: navEntry.modelData.label
                            active: navEntry.modelData.key === root.navKey
                            collapsed: root.railMode
                            onClicked: root.selectNav(navEntry.modelData)
                        }
                    }
                }

                NavItem {
                    objectName: "settingsButton"
                    Layout.fillWidth: true
                    iconName: "settings"
                    label: "Settings"
                    collapsed: root.railMode
                    Accessible.description: "Completed-task cleanup"
                    onClicked: cleanup.open()
                }

                // footer: where the data lives, and the collapse toggle
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    spacing: Theme.s2

                    TNLabel {
                        visible: !root.railMode
                        Layout.fillWidth: true
                        role: "caption"
                        text: root.databasePath
                        elide: Text.ElideMiddle
                        color: Theme.textFaint
                        font.pixelSize: 11
                        ToolTip.visible: dbPathHover.hovered
                        ToolTip.text: "Database: " + root.databasePath
                        HoverHandler { id: dbPathHover }
                    }
                    TNIconButton {
                        objectName: "shortcutsButton"
                        iconName: "keyboard"
                        tip: "Keyboard shortcuts (F1)"
                        onClicked: shortcutsDialog.open()
                    }
                    TNIconButton {
                        visible: root.width >= Theme.railBreakpoint
                        Layout.alignment: Qt.AlignHCenter
                        iconName: "panel"
                        tip: root.forceRail ? "Expand sidebar" : "Collapse sidebar"
                        onClicked: root.forceRail = !root.forceRail
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: Theme.border
        }

        // Workspace
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.surface

            ColumnLayout {
                id: contentColumn
                objectName: "contentColumn"
                anchors.fill: parent
                anchors.leftMargin: Theme.s6
                anchors.rightMargin: Theme.s6
                anchors.topMargin: Theme.s5 + 4
                anchors.bottomMargin: Theme.s3
                spacing: Theme.s3

                // heading
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    spacing: Theme.s3

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        TNLabel {
                            objectName: "viewTitle"
                            role: "display"
                            text: root.currentNav.label
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        TNLabel {
                            objectName: "viewSubtitle"
                            role: "caption"
                            text: root.subtitle()
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        implicitWidth: countLabel.implicitWidth + 20
                        implicitHeight: 26
                        radius: 13
                        color: Theme.surfaceRaised
                        border.color: Theme.border
                        TNLabel {
                            id: countLabel
                            objectName: "taskCount"
                            anchors.centerIn: parent
                            role: "caption"
                            font.weight: Font.Medium
                            text: root.taskModel
                                  ? root.taskModel.count + (root.taskModel.count === 1 ? " task" : " tasks") : ""
                        }
                    }
                }

                // search + filter
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    Layout.topMargin: Theme.s1
                    spacing: Theme.s2

                    TNTextField {
                        id: searchField
                        objectName: "searchField"
                        Layout.fillWidth: true
                        iconName: "search"
                        clearable: true
                        placeholderText: "Search tasks   (Ctrl+F)"
                        onTextChanged: root.taskModel.searchText = text
                        Keys.onEscapePressed: clear()
                        Keys.onDownPressed: root.focusList()
                    }
                    TNComboBox {
                        id: filterBox
                        objectName: "filterBox"
                        Layout.preferredWidth: 150
                        model: ["All", "Active", "Completed"]
                        enabled: !root.completionFilterLocked
                        currentIndex: root.taskModel ? root.taskModel.completionFilter : 0
                        onActivated: (index) => {
                            root.errorText = ""
                            root.taskModel.completionFilter = index
                        }
                        Accessible.name: "Completion filter"
                    }
                }

                // add a task
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    spacing: Theme.s2

                    TNTextField {
                        id: addField
                        objectName: "addField"
                        Layout.fillWidth: true
                        iconName: "plus"
                        enabled: root.canAdd
                        invalid: root.addMessage !== ""
                        placeholderText: root.canAdd ? "Add a task and press Enter   (Ctrl+N)"
                                                     : "Switch to another view to add tasks"
                        onAccepted: root.addTask()
                        onTextEdited: root.addMessage = ""
                        Keys.onDownPressed: root.focusList()

                        // Shown inside the field so the list below never jumps.
                        TNLabel {
                            objectName: "addMessage"
                            visible: root.addMessage !== ""
                            text: root.addMessage
                            role: "caption"
                            color: Theme.danger
                            font.weight: Font.Medium
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.s3
                            anchors.verticalCenter: parent.verticalCenter
                            Accessible.role: Accessible.AlertMessage
                        }
                    }
                    TNButton {
                        objectName: "addButton"
                        variant: "primary"
                        text: "Add task"
                        enabled: root.canAdd
                        onClicked: root.addTask()
                    }
                    TNIconButton {
                        objectName: "quickAddToggle"
                        enabled: root.canAdd
                        iconName: "chevron"
                        rotation: root.quickDetails ? 180 : 0
                        tip: root.quickDetails ? "Hide details" : "Add details: priority, list, due date, reminder (Ctrl+Shift+N)"
                        Behavior on rotation { NumberAnimation { duration: Theme.durationFast } }
                        onClicked: root.quickDetails = !root.quickDetails

                        // a dot shows that hidden details are filled in
                        Rectangle {
                            visible: quickPanel.hasDetails && !root.quickDetails
                            width: 8
                            height: 8
                            radius: 4
                            color: Theme.accent
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 3
                        }
                    }
                }

                QuickAddPanel {
                    id: quickPanel
                    objectName: "quickAddPanel"
                    Layout.fillWidth: true
                    expanded: root.quickDetails && root.canAdd
                    onSubmitted: root.addTask()
                }

                // Fired reminders (in-app notification; stays until handled).
                Repeater {
                    model: reminders
                    delegate: ReminderBanner {
                        id: reminderBanner
                        required property var model
                        objectName: "reminderBanner"
                        Layout.fillWidth: true
                        title: reminderBanner.model.title
                        when: reminderBanner.model.when
                        onCompleteClicked: root.dismissReminder(reminderBanner.model.taskId, true)
                        onDismissClicked: root.dismissReminder(reminderBanner.model.taskId, false)
                    }
                }

                // Operation errors from the model/database.
                Rectangle {
                    objectName: "errorBanner"
                    Layout.fillWidth: true
                    visible: root.errorText !== ""
                    implicitHeight: errorRow.implicitHeight + 16
                    radius: Theme.radiusMd
                    color: Theme.dangerSoft
                    border.color: Theme.dangerLine

                    RowLayout {
                        id: errorRow
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s3
                        anchors.rightMargin: Theme.s2
                        anchors.topMargin: 8
                        anchors.bottomMargin: 8
                        spacing: Theme.s3

                        Icon { name: "warning"; size: 18; color: Theme.danger; Layout.alignment: Qt.AlignTop }
                        TNLabel {
                            objectName: "errorBannerText"
                            Layout.fillWidth: true
                            text: root.errorText
                            wrapMode: Text.Wrap
                            Accessible.role: Accessible.AlertMessage
                        }
                        TNButton {
                            objectName: "dismissBanner"
                            variant: "ghost"
                            text: "Dismiss"
                            implicitHeight: 30
                            onClicked: root.errorText = ""
                        }
                    }
                }

                // Task list
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ListView {
                        id: taskList
                        objectName: "taskList"
                        anchors.fill: parent
                        // The scrollbar lives in the right margin, so rows end
                        // exactly where the controls above them end.
                        anchors.rightMargin: -Theme.s5
                        clip: true
                        model: root.taskModel
                        boundsBehavior: Flickable.StopAtBounds
                        keyNavigationEnabled: true
                        ScrollBar.vertical: TNScrollBar {}

                        delegate: TaskRow {
                            // Full width, leaving room for the scrollbar only when it is shown.
                            width: taskList.width - Theme.s5
                            showList: root.currentNav.scope !== root.scopeList
                            compact: width < 640
                            onEditRequested: editor.openFor(model)
                            onDeleteRequested: root.askDelete(model)
                            onFocusNextRequested: root.focusRow(index + 1)
                            onFocusPreviousRequested: root.focusRow(index - 1)
                            onCompletionToggled: (completed) => {
                                root.errorText = ""
                                root.taskModel.setCompleted(model.id, completed)
                            }
                        }
                    }

                    EmptyState {
                        objectName: "emptyState"
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 40, 420)
                        visible: root.taskModel && root.taskModel.count === 0
                        title: root.taskModel ? root.emptyTitle() : ""
                        text: root.taskModel ? root.emptyMessage() : ""
                        iconName: root.taskModel ? root.emptyIcon() : "check"
                        accented: root.taskModel ? root.emptyAccented() : true
                    }
                }
            }

            TNToast {
                id: toast
                objectName: "toast"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.s5
                width: Math.min(implicitWidth, parent.width - Theme.s5 * 2)
                onActionTriggered: root.undoDelete()
                onExpired: {
                    if (root.undoToastActive) {
                        root.undoToastActive = false
                        root.taskModel.discardUndo() // the window to undo has passed
                    }
                }
            }
        }
    }

    // ---- dialogs ---------------------------------------------------------
    function askDelete(taskRow) {
        deleteDialog.taskId = taskRow.id
        deleteDialog.taskTitle = taskRow.title
        deleteDialog.rowIndex = taskRow.index !== undefined ? taskRow.index : -1
        deleteDialog.open()
    }

    TaskEditor {
        id: editor
        objectName: "taskEditor"
        taskModel: root.taskModel
    }

    CleanupSettings {
        id: cleanup
        objectName: "cleanupSettings"
        taskModel: root.taskModel
        onNotice: (text) => root.notify(text, "check")
    }

    TNDialog {
        id: shortcutsDialog
        objectName: "shortcutsDialog"
        title: "Keyboard shortcuts"
        width: Math.min(460, parent ? parent.width - 32 : 460)
        onOpened: shortcutsCloseButton.forceActiveFocus() // Enter/Space closes it

        readonly property var entries: [
            { keys: "Ctrl+N", what: "New task (focus the add field)" },
            { keys: "Ctrl+Shift+N", what: "New task with details (priority, list, due, reminder)" },
            { keys: "Ctrl+Z", what: "Undo a delete, while the notice is showing" },
            { keys: "Ctrl+F", what: "Search tasks" },
            { keys: "Ctrl+1 \u2013 5", what: "All Tasks, Today, Upcoming, Completed, Inbox" },
            { keys: "\u2193 / \u2191", what: "From search or add: into the list; between tasks" },
            { keys: "Space", what: "Complete or reopen the focused task" },
            { keys: "Enter", what: "Edit the focused task (or add, in the add field)" },
            { keys: "Delete", what: "Delete the focused task (asks first)" },
            { keys: "Ctrl+Enter", what: "Save, while editing notes" },
            { keys: "Esc", what: "Close a dialog without saving; clear search" },
            { keys: "F1", what: "This list" }
        ]

        contentItem: ColumnLayout {
            spacing: Theme.s2

            Repeater {
                model: shortcutsDialog.entries
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.s3

                    Rectangle {
                        Layout.preferredWidth: 116
                        implicitHeight: 26
                        radius: Theme.radiusSm
                        color: Theme.surfaceRaised
                        border.color: Theme.borderStrong
                        TNLabel {
                            anchors.centerIn: parent
                            role: "caption"
                            font.weight: Font.DemiBold
                            text: modelData.keys
                        }
                    }
                    TNLabel {
                        Layout.fillWidth: true
                        role: "label"
                        color: Theme.text
                        wrapMode: Text.Wrap
                        text: modelData.what
                    }
                }
            }
        }

        footer: Item {
            implicitHeight: 64
            TNButton {
                id: shortcutsCloseButton
                objectName: "shortcutsClose"
                anchors.right: parent.right
                anchors.rightMargin: Theme.s5
                anchors.verticalCenter: parent.verticalCenter
                text: "Close"
                onClicked: shortcutsDialog.close()
            }
        }
    }

    TNDialog {
        id: deleteDialog
        objectName: "deleteDialog"

        property var taskId: 0
        property string taskTitle: ""
        property int rowIndex: -1       // where the task was, for focus afterwards
        property int focusRowAfter: -1  // set when a delete went through
        property bool closing: false

        onAboutToShow: closing = false
        onAboutToHide: closing = true
        onClosed: {
            // The deleted row's focus is gone: put it on the neighbouring task
            // (or search), so keyboard use can continue without the mouse.
            if (focusRowAfter >= 0) {
                const target = Math.min(focusRowAfter, root.taskModel.count - 1)
                focusRowAfter = -1
                if (target >= 0)
                    root.focusRow(target)
                else
                    searchField.forceActiveFocus()
            }
        }

        title: "Delete task?"
        width: Math.min(420, parent ? parent.width - 32 : 420)

        onOpened: deleteCancel.forceActiveFocus() // the safe choice is the default

        contentItem: TNLabel {
            wrapMode: Text.Wrap
            color: Theme.textMuted
            text: "“" + deleteDialog.taskTitle + "” will be permanently deleted. "
                  + "This can’t be undone."
        }

        footer: Item {
            implicitHeight: 64

            Row {
                anchors.right: parent.right
                anchors.rightMargin: Theme.s5
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.s2

                TNButton {
                    id: deleteCancel
                    objectName: "deleteCancel"
                    variant: "ghost"
                    text: "Cancel"
                    onClicked: deleteDialog.close()
                }
                TNButton {
                    objectName: "deleteConfirm"
                    variant: "danger"
                    iconName: "trash"
                    text: "Delete"
                    onClicked: {
                        // A second click while the dialog is closing must not
                        // delete again (it would report "not found").
                        if (deleteDialog.closing || !deleteDialog.visible)
                            return
                        deleteDialog.closing = true
                        root.errorText = ""
                        const deletedTitle = deleteDialog.taskTitle
                        const ok = root.taskModel.removeTask(deleteDialog.taskId)
                        deleteDialog.close()
                        if (ok) {
                            deleteDialog.focusRowAfter = Math.max(deleteDialog.rowIndex, 0)
                            root.offerUndo(deletedTitle)
                        }
                    }
                }
            }
        }
    }
}
