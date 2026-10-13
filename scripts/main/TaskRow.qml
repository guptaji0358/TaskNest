import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One task. Layout (left to right): completion ring | title + notes preview |
// list | priority | due | reminder | edit / delete. The row itself is only
// activated by double-click or Enter (edit) and Delete (delete), so the
// controls inside it never compete with a row-wide click target.
ItemDelegate {
    id: row

    required property var model
    required property int index

    property bool showList: true   // show the list name when it is not Inbox
    property bool compact: false   // narrow: drop secondary metadata

    signal editRequested()
    signal deleteRequested()
    signal completionToggled(bool completed)

    // Bumped by the one-shot timer below when the due time passes, so a task
    // turns "Overdue" while the window stays open (no polling).
    property int tick: 0
    readonly property var due: { row.tick; return Theme.dueInfo(model.dueAt, model.completed) }
    readonly property bool hasReminder: model.reminderAt !== undefined && model.reminderAt !== null
                                        && !isNaN(model.reminderAt.getTime()) && !model.reminderSent
    readonly property bool engaged: hoverHandler.hovered || visualFocus
                                    || editButton.visualFocus || deleteButton.visualFocus

    objectName: "taskRow"
    implicitHeight: Theme.rowHeight
    padding: 0
    leftPadding: Theme.s2
    rightPadding: Theme.s2
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: model.title

    Keys.onReturnPressed: row.editRequested()
    Keys.onDeletePressed: row.deleteRequested()
    onDoubleClicked: row.editRequested()

    HoverHandler { id: hoverHandler }

    Timer {
        repeat: false
        running: row.due !== null && row.due.state !== "overdue" && row.due.state !== "done"
        interval: row.due === null ? 1
                  : Math.min(Math.max(row.model.dueAt.getTime() - Date.now(), 0) + 250, 2147483647)
        onTriggered: row.tick++
    }

    background: Rectangle {
        radius: Theme.radiusSm
        color: row.down ? Theme.surfaceRaised : (row.engaged ? Theme.surfaceHover : "transparent")
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        // keyboard focus: accent bar on the leading edge
        Rectangle {
            visible: row.visualFocus
            width: 3
            height: parent.height - 16
            radius: 2
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.focus
        }
        // hairline separator instead of a card per task
        Rectangle {
            visible: !row.engaged
            height: 1
            color: Theme.border
            anchors.left: parent.left
            anchors.leftMargin: 44
            anchors.right: parent.right
            anchors.rightMargin: Theme.s2
            anchors.bottom: parent.bottom
        }
    }

    contentItem: RowLayout {
        spacing: Theme.s3

        TaskCheck {
            id: check
            objectName: "taskCheck"
            checked: row.model.completed
            Accessible.name: "Completed: " + row.model.title
            onToggled: {
                row.completionToggled(checked)
                // Follow the stored value, whether or not the write succeeded.
                checked = Qt.binding(() => row.model.completed)
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 1

            TNLabel {
                objectName: "taskTitle"
                role: "title"
                Layout.fillWidth: true
                text: row.model.title
                elide: Text.ElideRight
                font.strikeout: row.model.completed
                opacity: row.model.completed ? 0.55 : 1
            }
            TNLabel {
                visible: !row.compact && row.model.notes !== ""
                role: "caption"
                Layout.fillWidth: true
                text: row.model.notes.replace(/\s+/g, " ")
                elide: Text.ElideRight
                color: Theme.textFaint
            }
        }

        // list chip (only for lists other than Inbox)
        Row {
            visible: row.showList && !row.compact && row.model.listName !== "Inbox"
            spacing: 5
            Layout.alignment: Qt.AlignVCenter
            Icon { name: "list"; size: 13; color: Theme.textFaint; anchors.verticalCenter: parent.verticalCenter }
            TNLabel {
                role: "caption"
                text: row.model.listName
                color: Theme.textMuted
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        // priority: flag icon + word, so it is not color-only
        Rectangle {
            objectName: "priorityBadge"
            visible: row.model.priority > 0
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: priorityRow.implicitWidth + 14
            implicitHeight: 22
            radius: 11
            color: row.model.priority === 2 ? Theme.dangerSoft : Theme.warningSoft
            border.width: 1
            border.color: row.model.priority === 2 ? Theme.dangerLine : Qt.rgba(0.96, 0.71, 0.27, 0.4)
            opacity: row.model.completed ? 0.5 : 1

            Row {
                id: priorityRow
                anchors.centerIn: parent
                spacing: 4
                Icon {
                    name: "flag"
                    size: 12
                    strokeWidth: 2
                    color: row.model.priority === 2 ? Theme.danger : Theme.warning
                    anchors.verticalCenter: parent.verticalCenter
                }
                TNLabel {
                    role: "caption"
                    text: row.model.priority === 2 ? "High" : "Medium"
                    font.weight: Font.DemiBold
                    color: row.model.priority === 2 ? Theme.danger : Theme.warning
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // due date: icon + words carry the urgency, color reinforces it
        Row {
            objectName: "dueLabel"
            visible: row.due !== null
            spacing: 5
            Layout.alignment: Qt.AlignVCenter
            Icon {
                name: row.due ? Theme.dueIcon(row.due.state) : ""
                size: 14
                color: row.due ? Theme.dueColor(row.due.state) : Theme.textMuted
                anchors.verticalCenter: parent.verticalCenter
            }
            TNLabel {
                objectName: "dueText"
                role: "caption"
                text: row.due ? row.due.text : ""
                color: row.due ? Theme.dueColor(row.due.state) : Theme.textMuted
                font.weight: row.due && (row.due.state === "overdue" || row.due.state === "today")
                             ? Font.DemiBold : Font.Normal
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        // pending reminder
        Row {
            objectName: "reminderLabel"
            visible: row.hasReminder
            spacing: 5
            Layout.alignment: Qt.AlignVCenter
            Icon { name: "bell"; size: 14; color: Theme.secondary; anchors.verticalCenter: parent.verticalCenter }
            TNLabel {
                visible: !row.compact
                role: "caption"
                text: row.hasReminder ? Theme.reminderText(row.model.reminderAt) : ""
                color: Theme.secondary
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            Layout.alignment: Qt.AlignVCenter
            spacing: 2
            opacity: row.engaged ? 1 : 0.8
            Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

            TNIconButton {
                id: editButton
                objectName: "editButton"
                iconName: "edit"
                tip: "Edit task"
                Accessible.name: "Edit task: " + row.model.title
                onClicked: row.editRequested()
            }
            TNIconButton {
                id: deleteButton
                objectName: "deleteButton"
                iconName: "trash"
                tip: "Delete task"
                danger: true
                Accessible.name: "Delete task: " + row.model.title
                onClicked: row.deleteRequested()
            }
        }
    }
}
