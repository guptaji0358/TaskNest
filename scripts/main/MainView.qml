import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The task interface. All data access goes through `taskModel`
// (TaskListModel); nothing here touches SQL.
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

    // Explicit accent colors: the system "highlight" role can be near-black.
    readonly property color accent: "#2f6fd0"
    readonly property color textOnAccent: "white"
    readonly property color outline: Qt.rgba(palette.windowText.r, palette.windowText.g,
                                             palette.windowText.b, 0.45)

    property string navKey: "all"
    property string errorText: ""   // last model error, shown in the banner
    property string addMessage: ""  // validation message under the add field

    // Sidebar entries: fixed views, then the lists found in the data.
    readonly property var navItems: {
        const items = [
            { key: "all", label: "All Tasks", scope: scopeEverything, list: "", completion: filterAll },
            { key: "today", label: "Today", scope: scopeToday, list: "", completion: filterAll },
            { key: "upcoming", label: "Upcoming", scope: scopeUpcoming, list: "", completion: filterActive },
            { key: "completed", label: "Completed", scope: scopeEverything, list: "", completion: filterCompleted },
            { key: "list:Inbox", label: "Inbox", scope: scopeList, list: "Inbox", completion: filterAll }
        ]
        const names = taskModel ? taskModel.listNames : []
        let custom = []
        for (let i = 0; i < names.length; ++i) {
            if (names[i] !== "Inbox")
                custom.push({ key: "list:" + names[i], label: names[i], scope: scopeList,
                              list: names[i], completion: filterAll })
        }
        if (custom.length > 0)
            items.push({ key: "header", label: "LISTS", header: true })
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

    function selectNav(item) {
        errorText = ""
        if (taskModel.setView(item.scope, item.list, item.completion))
            navKey = item.key
    }

    function addTask() {
        errorText = ""
        const title = addField.text.trim()
        if (title === "") {
            addMessage = "Enter a task title first."
            addField.forceActiveFocus()
            return
        }
        addMessage = ""

        // Give the new task what the current view needs to show it.
        let due = new Date(NaN)
        if (navKey === "today" || navKey === "upcoming") {
            due = new Date()
            if (navKey === "upcoming")
                due.setDate(due.getDate() + 1)
            due.setHours(23, 59, 0, 0)
        }
        const list = currentNav.scope === scopeList ? currentNav.list : ""

        if (taskModel.addTask(title, "", list, 0, due)) {
            addField.clear()
            if (searchField.text !== "")
                searchField.clear() // otherwise the new task could be hidden
        } // on failure the banner shows the error and the text is kept
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
        function onListNamesChanged() {
            // The list being viewed no longer has tasks: fall back to All Tasks.
            if (root.navKey.startsWith("list:") && root.navKey !== "list:Inbox"
                    && root.taskModel.listNames.indexOf(root.navKey.substring(5)) < 0)
                root.selectNav(root.navItems[0])
        }
    }

    Shortcut {
        sequence: "Ctrl+N"
        enabled: root.canAdd
        onActivated: addField.forceActiveFocus()
    }
    Shortcut {
        sequence: "Ctrl+F"
        enabled: root.databaseOk
        onActivated: searchField.forceActiveFocus()
    }

    // ---- database unavailable -------------------------------------------
    ColumnLayout {
        visible: !root.databaseOk
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 640)
        spacing: 12

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "TaskNest"
            font.pixelSize: 40
            font.bold: true
        }
        Label {
            objectName: "unavailableMessage"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: "#c62828"
            text: "Database unavailable. Your data has not been modified.\n" + root.databaseError
            Accessible.role: Accessible.AlertMessage
        }
    }

    // ---- main interface -------------------------------------------------
    RowLayout {
        visible: root.databaseOk
        anchors.fill: parent
        spacing: 0

        // Sidebar
        Rectangle {
            Layout.preferredWidth: 210
            Layout.fillHeight: true
            // Slightly offset from the window color; avoids accent-tinted palette roles.
            color: Qt.darker(root.palette.window, 1.12)

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Label {
                    text: "TaskNest"
                    font.pixelSize: 22
                    font.bold: true
                    Layout.leftMargin: 6
                }

                ListView {
                    id: navList
                    objectName: "navList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.navItems
                    delegate: ItemDelegate {
                        id: navDelegate
                        required property var modelData
                        objectName: "nav_" + modelData.key
                        width: ListView.view.width
                        text: modelData.label
                        enabled: !modelData.header
                        highlighted: modelData.key === root.navKey
                        font.pixelSize: modelData.header ? 11 : 14
                        font.bold: modelData.header === true
                        topPadding: modelData.header ? 14 : 6
                        bottomPadding: 6
                        background: Rectangle {
                            radius: 4
                            color: navDelegate.highlighted ? root.accent
                                   : navDelegate.hovered ? Qt.rgba(root.palette.windowText.r,
                                                                   root.palette.windowText.g,
                                                                   root.palette.windowText.b, 0.08)
                                                         : "transparent"
                            border.color: root.accent
                            border.width: navDelegate.visualFocus ? 2 : 0
                        }
                        contentItem: Label {
                            text: navDelegate.text
                            font: navDelegate.font
                            color: navDelegate.highlighted ? root.textOnAccent
                                                           : root.palette.windowText
                            opacity: navDelegate.modelData.header ? 0.6 : 1
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: root.selectNav(modelData)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: root.databasePath
                    elide: Text.ElideMiddle
                    font.pixelSize: 10
                    opacity: 0.55
                    ToolTip.visible: dbPathHover.hovered
                    ToolTip.text: "Database: " + root.databasePath
                    HoverHandler { id: dbPathHover }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: root.palette.mid
            opacity: 0.5
        }

        // Content
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 20
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                Label {
                    objectName: "viewTitle"
                    text: root.currentNav.label
                    font.pixelSize: 26
                    font.bold: true
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "taskCount"
                    text: root.taskModel ? root.taskModel.count + (root.taskModel.count === 1 ? " task" : " tasks") : ""
                    opacity: 0.65
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: searchField
                    objectName: "searchField"
                    Layout.fillWidth: true
                    placeholderText: "Search tasks (Ctrl+F)"
                    selectByMouse: true
                    onTextChanged: root.taskModel.searchText = text
                    Keys.onEscapePressed: clear()
                }
                ComboBox {
                    id: filterBox
                    objectName: "filterBox"
                    Layout.preferredWidth: 130
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

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: addField
                    objectName: "addField"
                    Layout.fillWidth: true
                    enabled: root.canAdd
                    placeholderText: root.canAdd ? "Add a task and press Enter (Ctrl+N)"
                                                 : "Switch to another view to add tasks"
                    selectByMouse: true
                    onAccepted: root.addTask()
                    onTextEdited: root.addMessage = ""
                }
                Button {
                    objectName: "addButton"
                    text: "Add Task"
                    highlighted: true
                    enabled: root.canAdd
                    onClicked: root.addTask()
                }
            }

            Label {
                objectName: "addMessage"
                Layout.fillWidth: true
                visible: root.addMessage !== ""
                text: root.addMessage
                color: "#c62828"
                Accessible.role: Accessible.AlertMessage
            }

            // Fired reminders (in-app notification; stays until handled).
            Repeater {
                model: reminders
                delegate: Rectangle {
                    id: reminderBanner
                    required property var model
                    objectName: "reminderBanner"
                    Layout.fillWidth: true
                    implicitHeight: reminderRow.implicitHeight + 16
                    radius: 4
                    color: root.accent

                    RowLayout {
                        id: reminderRow
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8
                        Label {
                            objectName: "reminderText"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: root.textOnAccent
                            font.bold: true
                            text: "Reminder: " + reminderBanner.model.title + "  ("
                                  + Qt.formatDateTime(reminderBanner.model.when, "ddd d MMM, HH:mm") + ")"
                            Accessible.role: Accessible.AlertMessage
                        }
                        Button {
                            objectName: "reminderComplete"
                            text: "Complete"
                            onClicked: root.dismissReminder(reminderBanner.model.taskId, true)
                        }
                        Button {
                            objectName: "reminderDismiss"
                            text: "Dismiss"
                            onClicked: root.dismissReminder(reminderBanner.model.taskId, false)
                        }
                    }
                }
            }

            // Operation errors from the model/database.
            Rectangle {
                objectName: "errorBanner"
                Layout.fillWidth: true
                visible: root.errorText !== ""
                implicitHeight: bannerRow.implicitHeight + 16
                radius: 4
                color: "#fdecea"
                border.color: "#c62828"

                RowLayout {
                    id: bannerRow
                    anchors.fill: parent
                    anchors.margins: 8
                    Label {
                        objectName: "errorBannerText"
                        Layout.fillWidth: true
                        text: root.errorText
                        color: "#7f1d1d"
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                    Button {
                        objectName: "dismissBanner"
                        text: "Dismiss"
                        flat: true
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
                    clip: true
                    spacing: 2
                    model: root.taskModel
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    delegate: ItemDelegate {
                        id: row
                        required property var model
                        required property int index
                        objectName: "taskRow"
                        width: ListView.view.width - 12
                        readonly property bool hasDue: model.dueAt !== undefined && model.dueAt !== null
                                                       && !isNaN(model.dueAt.getTime())
                        // Self-contained (does not rely on hasDue being re-evaluated first).
                        readonly property bool overdue: !model.completed
                                                        && model.dueAt !== undefined
                                                        && model.dueAt !== null
                                                        && model.dueAt.getTime() < Date.now()

                        onDoubleClicked: editor.openFor(row.model)
                        Keys.onReturnPressed: editor.openFor(row.model)
                        Keys.onDeletePressed: root.askDelete(row.model)

                        contentItem: RowLayout {
                            spacing: 10

                            CheckBox {
                                id: check
                                objectName: "taskCheck"
                                checked: row.model.completed
                                indicator: Rectangle {
                                    implicitWidth: 20
                                    implicitHeight: 20
                                    x: check.leftPadding
                                    y: (check.height - height) / 2
                                    radius: 3
                                    color: check.checked ? root.accent : "transparent"
                                    border.color: check.visualFocus || check.checked
                                                  ? root.accent : root.outline
                                    border.width: check.visualFocus ? 2 : 1
                                    Label {
                                        anchors.centerIn: parent
                                        visible: check.checked
                                        text: "✓"
                                        font.bold: true
                                        color: root.textOnAccent
                                    }
                                }
                                Accessible.name: "Completed: " + row.model.title
                                onToggled: {
                                    root.errorText = ""
                                    root.taskModel.setCompleted(row.model.id, checked)
                                    // Follow the stored value, whether or not the write succeeded.
                                    checked = Qt.binding(() => row.model.completed)
                                }
                            }

                            Label {
                                objectName: "taskTitle"
                                Layout.fillWidth: true
                                text: row.model.title
                                elide: Text.ElideRight
                                font.pixelSize: 15
                                font.strikeout: row.model.completed
                                opacity: row.model.completed ? 0.5 : 1
                            }

                            Rectangle {
                                objectName: "priorityBadge"
                                visible: row.model.priority > 0
                                implicitWidth: priorityLabel.implicitWidth + 12
                                implicitHeight: priorityLabel.implicitHeight + 4
                                radius: 3
                                color: row.model.priority === 2 ? "#c62828" : "#b26a00"
                                opacity: row.model.completed ? 0.5 : 1
                                Label {
                                    id: priorityLabel
                                    anchors.centerIn: parent
                                    text: row.model.priority === 2 ? "High" : "Medium"
                                    color: "white"
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                            }

                            Label {
                                objectName: "dueLabel"
                                visible: row.hasDue
                                text: {
                                    if (!row.hasDue)
                                        return ""
                                    const d = row.model.dueAt
                                    const dateOnly = d.getHours() === 23 && d.getMinutes() === 59
                                    return Qt.formatDate(d, "ddd d MMM yyyy")
                                           + (dateOnly ? "" : ", " + Qt.formatTime(d, "HH:mm"))
                                }
                                color: row.overdue ? "#c62828" : row.palette.windowText
                                opacity: row.overdue ? 1 : 0.65
                                font.pixelSize: 12
                            }

                            Label {
                                objectName: "reminderLabel"
                                readonly property var at: row.model.reminderAt
                                visible: at !== undefined && at !== null && !isNaN(at.getTime())
                                         && !row.model.reminderSent
                                text: visible ? "\u23F0 " + Qt.formatDateTime(at, "d MMM, HH:mm") : ""
                                font.pixelSize: 12
                                opacity: 0.65
                            }

                            ToolButton {
                                objectName: "editButton"
                                text: "Edit"
                                onClicked: editor.openFor(row.model)
                                Accessible.name: "Edit task: " + row.model.title
                            }
                            ToolButton {
                                objectName: "deleteButton"
                                text: "Delete"
                                onClicked: root.askDelete(row.model)
                                Accessible.name: "Delete task: " + row.model.title
                            }
                        }
                    }
                }

                Label {
                    objectName: "emptyState"
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 40, 420)
                    visible: root.taskModel && root.taskModel.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    opacity: 0.6
                    font.pixelSize: 15
                    text: root.taskModel ? root.emptyMessage() : ""
                }
            }
        }
    }

    // ---- dialogs ---------------------------------------------------------
    function askDelete(taskRow) {
        deleteDialog.taskId = taskRow.id
        deleteDialog.taskTitle = taskRow.title
        deleteDialog.open()
    }

    TaskEditor {
        id: editor
        objectName: "taskEditor"
        taskModel: root.taskModel
    }

    Dialog {
        id: deleteDialog
        objectName: "deleteDialog"
        property var taskId: 0
        property string taskTitle: ""

        title: "Delete task?"
        modal: true
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(400, parent ? parent.width - 32 : 400)

        contentItem: Label {
            wrapMode: Text.Wrap
            text: "“" + deleteDialog.taskTitle + "” will be permanently deleted. "
                  + "This cannot be undone."
        }
        footer: DialogButtonBox {
            Button {
                objectName: "deleteConfirm"
                text: "Delete"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                objectName: "deleteCancel"
                text: "Cancel"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        onAccepted: {
            root.errorText = ""
            root.taskModel.removeTask(taskId)
        }
    }
}
