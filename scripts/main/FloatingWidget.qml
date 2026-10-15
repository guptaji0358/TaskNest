import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// The floating desktop widget: a compact, frameless, always-on-top tool window
// with today's and upcoming tasks, Quick Add and fired reminders. It owns no
// task logic: reads come from the controller's two read-only task views, and
// every change goes through `taskModel` (the main TaskListModel), so the
// widget and the main window cannot disagree.
Window {
    id: win

    required property var controller   // WidgetController
    required property var taskModel    // TaskListModel (main)
    property var remindersModel: null  // ListModel of fired reminders (taskId, title, when)

    // The main application is asked to do these; the widget never edits tasks itself.
    signal openTaskRequested(var taskId)
    signal openAppRequested()
    signal reminderDoneRequested(var taskId)
    signal reminderDismissRequested(var taskId)

    // For tests.
    readonly property alias quickField: quickAddField
    property string errorText: ""
    property bool placed: false

    readonly property bool compact: controller.compact
    readonly property int rowH: compact ? 34 : 46
    readonly property int maxTodayRows: compact ? 6 : 10
    readonly property int maxUpcomingRows: compact ? 4 : 6
    readonly property int reminderCount: remindersModel ? remindersModel.count : 0
    readonly property color surfaceColor: controller.surface === 1 ? Theme.bg
                                          : controller.surface === 2 ? Theme.elevated : Theme.surface
    readonly property bool anythingShown: controller.showToday || controller.showUpcoming
                                          || controller.showQuickAdd
                                          || (controller.showReminders && reminderCount > 0)

    // Not owned by the main window: it must stay up when that is minimized.
    transientParent: null
    visible: controller.widgetVisible
    title: "TaskNest widget"
    flags: Qt.Tool | Qt.FramelessWindowHint
           | (controller.alwaysOnTop ? Qt.WindowStaysOnTopHint : 0)
    color: "transparent"
    opacity: controller.opacity
    minimumWidth: 220
    minimumHeight: 200

    // Position and size are restored once, each time the widget is shown; a
    // saved position that is no longer on a screen is moved back onto one.
    onVisibleChanged: {
        if (visible && !placed) {
            width = controller.width
            height = controller.height
            const p = controller.recoveredPosition(width, height)
            if (p.valid) {
                x = p.x
                y = p.y
            }
            placed = true
        } else if (!visible) {
            placed = false
        }
    }
    onXChanged: { if (placed && visible) controller.x = x }
    onYChanged: { if (placed && visible) controller.y = y }
    onWidthChanged: { if (placed && visible) controller.width = width }
    onHeightChanged: { if (placed && visible) controller.height = height }

    // Alt+F4 or the system menu hides the widget; tasks and reminders are untouched.
    onClosing: (close) => {
        close.accepted = true
        controller.widgetVisible = false
    }

    function open(taskId) { win.openTaskRequested(taskId) }

    function addQuick() {
        const title = quickAddField.text.trim()
        if (title === "")
            return
        errorText = ""
        let due = new Date(NaN)
        if (dueChoice.currentIndex < 2) { // Today / Tomorrow: end of day, like a date-only due
            due = new Date()
            due.setDate(due.getDate() + dueChoice.currentIndex)
            due.setHours(23, 59, 0, 0)
        }
        if (taskModel.addTask(title, "", "", 0, due, new Date(NaN)))
            quickAddField.clear()
    }

    Connections {
        target: win.taskModel
        function onErrorOccurred(message) { win.errorText = message }
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: win.visible && !quickAddField.activeFocus
        onActivated: win.controller.widgetVisible = false
    }

    component EdgeGrip: MouseArea {
        property int edges: 0
        hoverEnabled: true
        cursorShape: (edges === (Qt.LeftEdge | Qt.TopEdge) || edges === (Qt.RightEdge | Qt.BottomEdge))
                     ? Qt.SizeFDiagCursor
                     : (edges === (Qt.RightEdge | Qt.TopEdge) || edges === (Qt.LeftEdge | Qt.BottomEdge))
                       ? Qt.SizeBDiagCursor
                       : (edges === Qt.LeftEdge || edges === Qt.RightEdge) ? Qt.SizeHorCursor
                                                                           : Qt.SizeVerCursor
        onPressed: win.startSystemResize(edges)
    }

    // One task: ring | title + details | open
    component WidgetTask: Item {
        id: task
        required property var model
        required property int index
        width: ListView.view ? ListView.view.width : 0
        height: win.rowH
        readonly property var due: Theme.dueInfo(model.dueAt, model.completed)

        HoverHandler { id: hover }
        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusSm
            color: hover.hovered || checkBox.visualFocus || openButton.visualFocus
                   ? Theme.surfaceRaised : "transparent"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 2
            anchors.rightMargin: 2
            spacing: Theme.s2

            TaskCheck {
                id: checkBox
                objectName: "widgetTaskCheck"
                checked: task.model.completed === true
                Accessible.name: "Complete: " + task.model.title
                onToggled: {
                    win.errorText = ""
                    win.taskModel.setCompleted(task.model.id, checked)
                    // follow the stored value if the write was refused
                    checked = Qt.binding(() => task.model.completed === true)
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                TNLabel {
                    objectName: "widgetTaskTitle"
                    Layout.fillWidth: true
                    role: "label"
                    color: Theme.text
                    elide: Text.ElideRight
                    text: task.model.title
                }
                Row {
                    visible: win.controller.showDetails && !win.compact
                             && (task.due !== null || task.model.priority > 0)
                    spacing: Theme.s2
                    Icon {
                        visible: task.model.priority > 0
                        name: "flag"
                        size: 12
                        color: task.model.priority === 2 ? Theme.danger : Theme.warning
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    TNLabel {
                        visible: task.model.priority > 0
                        role: "caption"
                        text: task.model.priority === 2 ? "High" : "Medium"
                        color: task.model.priority === 2 ? Theme.danger : Theme.warning
                    }
                    Icon {
                        visible: task.due !== null
                        name: task.due ? Theme.dueIcon(task.due.state) : ""
                        size: 12
                        color: task.due ? Theme.dueColor(task.due.state) : Theme.textMuted
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    TNLabel {
                        objectName: "widgetTaskDue"
                        visible: task.due !== null
                        role: "caption"
                        text: task.due ? task.due.text : ""
                        color: task.due ? Theme.dueColor(task.due.state) : Theme.textMuted
                    }
                }
            }

            TNIconButton {
                id: openButton
                objectName: "widgetTaskOpen"
                iconName: "edit"
                tip: "Open in TaskNest"
                onClicked: win.open(task.model.id)
            }
        }

        TapHandler {
            acceptedButtons: Qt.LeftButton
            onDoubleTapped: win.open(task.model.id)
        }
    }

    // Themed check box row (the stock indicator ignores the palette).
    component OptionCheck: CheckBox {
        id: option
        implicitHeight: 28
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        indicator: Rectangle {
            y: (option.height - height) / 2
            width: 18
            height: 18
            radius: 5
            color: option.checked ? Theme.accent : "transparent"
            border.width: option.visualFocus ? 2 : 1.5
            border.color: option.visualFocus ? Theme.focus
                          : option.checked ? Theme.accent
                          : option.hovered ? Theme.textMuted : Theme.borderStrong
            Icon {
                anchors.centerIn: parent
                visible: option.checked
                name: "check"
                size: 12
                strokeWidth: 2.6
                color: Theme.accentText
            }
        }
        contentItem: Text {
            text: option.text
            leftPadding: 18 + 8
            verticalAlignment: Text.AlignVCenter
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontLabel
            elide: Text.ElideRight
        }
    }

    component SectionHeader: RowLayout {
        property string label: ""
        property int count: 0
        Layout.fillWidth: true
        spacing: Theme.s2
        TNLabel { role: "overline"; text: parent.label; Layout.alignment: Qt.AlignVCenter }
        Rectangle { height: 1; Layout.fillWidth: true; color: Theme.border }
        TNLabel { role: "caption"; text: parent.count; Layout.alignment: Qt.AlignVCenter }
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Theme.radiusLg
        color: win.surfaceColor
        border.width: 1
        border.color: Theme.borderStrong

        // Controls inherit this palette (selection, popups, check boxes).
        palette.window: Theme.bg
        palette.windowText: Theme.text
        palette.base: Theme.surfaceRaised
        palette.text: Theme.text
        palette.button: Theme.surfaceRaised
        palette.buttonText: Theme.text
        palette.highlight: Theme.accent
        palette.highlightedText: Theme.accentText
        palette.mid: Theme.borderStrong
        palette.dark: Theme.border

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.s3
            spacing: Theme.s2

            // ---- title bar: drag handle, open app, settings, hide ----------
            Item {
                id: titleBar
                Layout.fillWidth: true
                implicitHeight: 34

                DragHandler {
                    target: null
                    onActiveChanged: { if (active) win.startSystemMove() }
                }

                Row {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.s2
                    Rectangle {
                        width: 22
                        height: 22
                        radius: 7
                        color: Theme.accent
                        Icon { anchors.centerIn: parent; name: "nest"; size: 15; strokeWidth: 2.2
                               color: Theme.accentText }
                    }
                    TNLabel {
                        anchors.verticalCenter: parent.verticalCenter
                        role: "title"
                        text: "TaskNest"
                        visible: win.width > 250
                    }
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    TNIconButton {
                        objectName: "widgetOpenApp"
                        iconName: "all"
                        tip: "Open TaskNest"
                        onClicked: win.openAppRequested()
                    }
                    TNIconButton {
                        objectName: "widgetSettingsButton"
                        iconName: "settings"
                        tip: "Widget settings"
                        onClicked: settingsPopup.opened ? settingsPopup.close() : settingsPopup.open()
                    }
                    TNIconButton {
                        objectName: "widgetHide"
                        iconName: "close"
                        tip: "Hide widget"
                        onClicked: win.controller.widgetVisible = false
                    }
                }
            }

            // ---- content --------------------------------------------------
            Flickable {
                id: scroller
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: content.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: TNScrollBar {}

                ColumnLayout {
                    id: content
                    width: scroller.width
                    spacing: Theme.s2

                    // fired reminders
                    ColumnLayout {
                        objectName: "widgetReminders"
                        Layout.fillWidth: true
                        visible: win.controller.showReminders && win.reminderCount > 0
                        spacing: Theme.s1
                        SectionHeader { label: "Reminders"; count: win.reminderCount }
                        Repeater {
                            model: win.remindersModel
                            delegate: Rectangle {
                                id: reminder
                                required property var model
                                Layout.fillWidth: true
                                implicitHeight: reminderRow.implicitHeight + 10
                                radius: Theme.radiusSm
                                color: Theme.secondarySoft
                                border.color: Theme.secondaryLine
                                RowLayout {
                                    id: reminderRow
                                    anchors.fill: parent
                                    anchors.margins: 5
                                    anchors.leftMargin: Theme.s2
                                    spacing: Theme.s1
                                    Icon { name: "bell"; size: 14; color: Theme.secondary }
                                    TNLabel {
                                        Layout.fillWidth: true
                                        role: "label"
                                        color: Theme.text
                                        elide: Text.ElideRight
                                        text: reminder.model.title
                                    }
                                    TNIconButton {
                                        objectName: "widgetReminderDone"
                                        iconName: "check"
                                        tip: "Mark done"
                                        onClicked: win.reminderDoneRequested(reminder.model.taskId)
                                    }
                                    TNIconButton {
                                        objectName: "widgetReminderDismiss"
                                        iconName: "close"
                                        tip: "Dismiss reminder"
                                        onClicked: win.reminderDismissRequested(reminder.model.taskId)
                                    }
                                }
                            }
                        }
                    }

                    // quick add
                    RowLayout {
                        Layout.fillWidth: true
                        visible: win.controller.showQuickAdd
                        spacing: Theme.s1
                        TNTextField {
                            id: quickAddField
                            objectName: "widgetQuickAdd"
                            Layout.fillWidth: true
                            implicitHeight: win.compact ? 32 : Theme.controlHeight
                            placeholderText: "Add a task"
                            Accessible.name: "Add a task"
                            onAccepted: win.addQuick()
                        }
                        TNComboBox {
                            id: dueChoice
                            objectName: "widgetDueChoice"
                            Layout.preferredWidth: 96
                            implicitHeight: win.compact ? 32 : Theme.controlHeight
                            visible: win.width > 260
                            model: ["Today", "Tomorrow", "No date"]
                            Accessible.name: "Due"
                        }
                    }

                    // today
                    ColumnLayout {
                        objectName: "widgetTodaySection"
                        Layout.fillWidth: true
                        visible: win.controller.showToday
                        spacing: Theme.s1
                        SectionHeader { label: "Today"; count: win.controller.todayModel.count }
                        ListView {
                            id: todayList
                            objectName: "widgetTodayList"
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(count, win.maxTodayRows) * win.rowH
                            interactive: false
                            clip: true
                            model: win.controller.todayModel
                            delegate: WidgetTask {}
                        }
                        TNLabel {
                            visible: win.controller.todayModel.count === 0
                            role: "caption"
                            text: "Nothing due today."
                        }
                        TNLabel {
                            visible: win.controller.todayModel.count > win.maxTodayRows
                            role: "caption"
                            text: "+" + (win.controller.todayModel.count - win.maxTodayRows)
                                  + " more in TaskNest"
                        }
                    }

                    // upcoming
                    ColumnLayout {
                        objectName: "widgetUpcomingSection"
                        Layout.fillWidth: true
                        visible: win.controller.showUpcoming
                        spacing: Theme.s1
                        SectionHeader { label: "Upcoming"; count: win.controller.upcomingModel.count }
                        ListView {
                            id: upcomingList
                            objectName: "widgetUpcomingList"
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(count, win.maxUpcomingRows) * win.rowH
                            interactive: false
                            clip: true
                            model: win.controller.upcomingModel
                            delegate: WidgetTask {}
                        }
                        TNLabel {
                            visible: win.controller.upcomingModel.count === 0
                            role: "caption"
                            text: "Nothing coming up."
                        }
                        TNLabel {
                            visible: win.controller.upcomingModel.count > win.maxUpcomingRows
                            role: "caption"
                            text: "+" + (win.controller.upcomingModel.count - win.maxUpcomingRows)
                                  + " more in TaskNest"
                        }
                    }

                    TNLabel {
                        objectName: "widgetNothingShown"
                        Layout.fillWidth: true
                        visible: !win.anythingShown
                        wrapMode: Text.Wrap
                        role: "caption"
                        text: "All sections are hidden. Use the settings button to show some."
                    }
                }
            }

            TNLabel {
                objectName: "widgetError"
                Layout.fillWidth: true
                visible: win.errorText !== ""
                role: "caption"
                color: Theme.danger
                wrapMode: Text.Wrap
                text: win.errorText
                Accessible.role: Accessible.AlertMessage
            }
        }

        // ---- settings -----------------------------------------------------
        Popup {
            id: settingsPopup
            objectName: "widgetSettings"
            x: Theme.s3
            y: 48
            width: frame.width - Theme.s3 * 2
            padding: Theme.s3
            margins: Theme.s2
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

            background: Rectangle {
                radius: Theme.radiusMd
                color: Theme.elevated
                border.color: Theme.borderStrong
            }

            contentItem: ColumnLayout {
                spacing: Theme.s1

                TNLabel { role: "overline"; text: "Sections" }
                Repeater {
                    model: [
                        { key: "showToday", label: "Today’s tasks" },
                        { key: "showUpcoming", label: "Upcoming tasks" },
                        { key: "showQuickAdd", label: "Quick add" },
                        { key: "showReminders", label: "Reminders" },
                        { key: "showDetails", label: "Due date and priority" }
                    ]
                    delegate: OptionCheck {
                        required property var modelData
                        objectName: "widgetOption_" + modelData.key
                        Layout.fillWidth: true
                        text: modelData.label
                        checked: win.controller[modelData.key]
                        onToggled: win.controller[modelData.key] = checked
                    }
                }

                TNLabel { role: "overline"; text: "Appearance" }
                RowLayout {
                    Layout.fillWidth: true
                    TNLabel { role: "label"; text: "Size"; Layout.fillWidth: true }
                    TNComboBox {
                        objectName: "widgetSizeBox"
                        Layout.preferredWidth: 130
                        model: ["Comfortable", "Compact"]
                        currentIndex: win.controller.compact ? 1 : 0
                        onActivated: (i) => win.controller.compact = (i === 1)
                        Accessible.name: "Widget size"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    TNLabel { role: "label"; text: "Surface"; Layout.fillWidth: true }
                    TNComboBox {
                        objectName: "widgetSurfaceBox"
                        Layout.preferredWidth: 130
                        model: ["Graphite", "Deep", "Raised"]
                        currentIndex: win.controller.surface
                        onActivated: (i) => win.controller.surface = i
                        Accessible.name: "Widget surface"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    TNLabel { role: "label"; text: "Opacity"; Layout.fillWidth: true }
                    Slider {
                        objectName: "widgetOpacity"
                        Layout.preferredWidth: 130
                        from: 0.6
                        to: 1.0
                        stepSize: 0.05
                        value: win.controller.opacity
                        onMoved: win.controller.opacity = value
                        Accessible.name: "Widget opacity"
                        background: Rectangle {
                            x: parent.leftPadding
                            y: parent.topPadding + parent.availableHeight / 2 - height / 2
                            width: parent.availableWidth
                            height: 4
                            radius: 2
                            color: Theme.borderStrong
                            Rectangle {
                                width: parent.width * parent.parent.visualPosition
                                height: parent.height
                                radius: 2
                                color: Theme.accent
                            }
                        }
                        handle: Rectangle {
                            x: parent.leftPadding + parent.visualPosition * (parent.availableWidth - width)
                            y: parent.topPadding + parent.availableHeight / 2 - height / 2
                            width: 16
                            height: 16
                            radius: 8
                            color: Theme.accent
                            border.width: parent.visualFocus ? 2 : 0
                            border.color: Theme.text
                        }
                    }
                }
                OptionCheck {
                    objectName: "widgetOption_alwaysOnTop"
                    Layout.fillWidth: true
                    text: "Keep above other windows"
                    checked: win.controller.alwaysOnTop
                    onToggled: win.controller.alwaysOnTop = checked
                }
            }
        }
    }

    // ---- resize edges ---------------------------------------------------------
    EdgeGrip { edges: Qt.LeftEdge; width: 5; anchors { left: parent.left; top: parent.top; bottom: parent.bottom; topMargin: 8; bottomMargin: 8 } }
    EdgeGrip { edges: Qt.RightEdge; width: 5; anchors { right: parent.right; top: parent.top; bottom: parent.bottom; topMargin: 8; bottomMargin: 8 } }
    EdgeGrip { edges: Qt.TopEdge; height: 5; anchors { top: parent.top; left: parent.left; right: parent.right; leftMargin: 8; rightMargin: 8 } }
    EdgeGrip { edges: Qt.BottomEdge; height: 5; anchors { bottom: parent.bottom; left: parent.left; right: parent.right; leftMargin: 8; rightMargin: 8 } }
    EdgeGrip { edges: Qt.LeftEdge | Qt.TopEdge; width: 10; height: 10; anchors { left: parent.left; top: parent.top } }
    EdgeGrip { edges: Qt.RightEdge | Qt.TopEdge; width: 10; height: 10; anchors { right: parent.right; top: parent.top } }
    EdgeGrip { edges: Qt.LeftEdge | Qt.BottomEdge; width: 10; height: 10; anchors { left: parent.left; bottom: parent.bottom } }
    EdgeGrip { edges: Qt.RightEdge | Qt.BottomEdge; width: 10; height: 10; anchors { right: parent.right; bottom: parent.bottom } }
}
