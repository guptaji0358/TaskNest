import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Modal editor for one task. Nothing is written until Save succeeds;
// Cancel / Escape leave the stored task untouched.
Dialog {
    id: editor

    required property var taskModel

    property var taskId: 0
    property string errorText: ""

    title: "Edit task"
    modal: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(460, parent ? parent.width - 32 : 460)
    closePolicy: Popup.CloseOnEscape

    // Fills the form from a row of the task model and opens the dialog.
    function openFor(row) {
        taskId = row.id
        titleField.text = row.title
        notesArea.text = row.notes
        listField.text = row.listName
        priorityBox.currentIndex = row.priority
        const due = row.dueAt
        if (due && !isNaN(due.getTime())) {
            dateField.text = Qt.formatDate(due, "yyyy-MM-dd")
            // Date-only tasks are stored as 23:59, so that time is left blank.
            const isDateOnly = due.getHours() === 23 && due.getMinutes() === 59
            timeField.text = isDateOnly ? "" : Qt.formatTime(due, "HH:mm")
        } else {
            dateField.text = ""
            timeField.text = ""
        }
        errorText = ""
        open()
        titleField.forceActiveFocus()
    }

    // Returns { error: string } or { due: Date } (invalid Date = no due date).
    function parseDue() {
        const dateText = dateField.text.trim()
        const timeText = timeField.text.trim()
        if (dateText === "" && timeText === "")
            return { due: new Date(NaN) }
        if (dateText === "")
            return { error: "Enter a due date, or clear the time." }

        const d = /^(\d{4})-(\d{2})-(\d{2})$/.exec(dateText)
        if (!d)
            return { error: "Due date must look like 2026-12-31." }
        let hours = 23
        let minutes = 59
        if (timeText !== "") {
            const t = /^(\d{1,2}):(\d{2})$/.exec(timeText)
            if (!t || Number(t[1]) > 23 || Number(t[2]) > 59)
                return { error: "Time must look like 14:30 (24-hour)." }
            hours = Number(t[1])
            minutes = Number(t[2])
        }
        const date = new Date(Number(d[1]), Number(d[2]) - 1, Number(d[3]), hours, minutes, 0, 0)
        if (date.getFullYear() !== Number(d[1]) || date.getMonth() !== Number(d[2]) - 1
                || date.getDate() !== Number(d[3]))
            return { error: "That date does not exist." }
        return { due: date }
    }

    function save() {
        errorText = ""
        if (titleField.text.trim() === "") {
            errorText = "A task title is required."
            titleField.forceActiveFocus()
            return
        }
        const parsed = parseDue()
        if (parsed.error) {
            errorText = parsed.error
            dateField.forceActiveFocus()
            return
        }
        const ok = taskModel.updateTask(taskId, titleField.text, notesArea.text,
                                        listField.text, priorityBox.currentIndex, parsed.due)
        if (ok)
            close()
        else
            errorText = taskModel.lastError // dialog stays open; nothing was saved
    }

    contentItem: ColumnLayout {
        spacing: 10

        Label { text: "Title" }
        TextField {
            id: titleField
            objectName: "editorTitle"
            Layout.fillWidth: true
            selectByMouse: true
            onAccepted: editor.save()
        }

        Label { text: "Notes" }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 90
            TextArea {
                id: notesArea
                objectName: "editorNotes"
                wrapMode: TextEdit.Wrap
                selectByMouse: true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                Label { text: "Priority" }
                ComboBox {
                    id: priorityBox
                    objectName: "editorPriority"
                    Layout.fillWidth: true
                    model: ["None", "Medium", "High"]
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: "List" }
                TextField {
                    id: listField
                    objectName: "editorList"
                    Layout.fillWidth: true
                    placeholderText: "Inbox"
                    selectByMouse: true
                    onAccepted: editor.save()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                Label { text: "Due date" }
                TextField {
                    id: dateField
                    objectName: "editorDate"
                    Layout.fillWidth: true
                    placeholderText: "YYYY-MM-DD"
                    selectByMouse: true
                    onAccepted: editor.save()
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: "Time (optional)" }
                TextField {
                    id: timeField
                    objectName: "editorTime"
                    Layout.fillWidth: true
                    placeholderText: "HH:MM"
                    selectByMouse: true
                    onAccepted: editor.save()
                }
            }
        }

        Label {
            objectName: "editorError"
            Layout.fillWidth: true
            visible: editor.errorText !== ""
            text: editor.errorText
            color: "#c62828"
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
    }

    footer: DialogButtonBox {
        Button {
            objectName: "editorSave"
            text: "Save"
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: editor.save()
        }
        Button {
            objectName: "editorCancel"
            text: "Cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }
}
