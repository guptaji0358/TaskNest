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
        const reminder = row.reminderAt
        if (reminder && !isNaN(reminder.getTime())) {
            reminderDateField.text = Qt.formatDate(reminder, "yyyy-MM-dd")
            reminderTimeField.text = Qt.formatTime(reminder, "HH:mm")
        } else {
            reminderDateField.text = ""
            reminderTimeField.text = ""
        }
        errorText = ""
        open()
        titleField.forceActiveFocus()
    }

    // Parses a local date + optional time. Both blank means "none".
    // Returns { error: string } or { value: Date } (invalid Date = none).
    // `label` names the field in messages; a missing time becomes hours:minutes.
    function parseDateTime(dateText, timeText, label, hours, minutes) {
        dateText = dateText.trim()
        timeText = timeText.trim()
        if (dateText === "" && timeText === "")
            return { value: new Date(NaN) }
        if (dateText === "")
            return { error: "Enter a " + label.toLowerCase() + " date, or clear the time." }

        const d = /^(\d{4})-(\d{2})-(\d{2})$/.exec(dateText)
        if (!d)
            return { error: label + " date must look like 2026-12-31." }
        if (timeText !== "") {
            const t = /^(\d{1,2}):(\d{2})$/.exec(timeText)
            if (!t || Number(t[1]) > 23 || Number(t[2]) > 59)
                return { error: label + " time must look like 14:30 (24-hour)." }
            hours = Number(t[1])
            minutes = Number(t[2])
        }
        const date = new Date(Number(d[1]), Number(d[2]) - 1, Number(d[3]), hours, minutes, 0, 0)
        if (date.getFullYear() !== Number(d[1]) || date.getMonth() !== Number(d[2]) - 1
                || date.getDate() !== Number(d[3]))
            return { error: "That " + label.toLowerCase() + " date does not exist." }
        return { value: date }
    }

    function save() {
        errorText = ""
        if (titleField.text.trim() === "") {
            errorText = "A task title is required."
            titleField.forceActiveFocus()
            return
        }
        // Date-only due dates mean end of day; date-only reminders mean 09:00.
        const due = parseDateTime(dateField.text, timeField.text, "Due", 23, 59)
        if (due.error) {
            errorText = due.error
            dateField.forceActiveFocus()
            return
        }
        const reminder = parseDateTime(reminderDateField.text, reminderTimeField.text,
                                       "Reminder", 9, 0)
        if (reminder.error) {
            errorText = reminder.error
            reminderDateField.forceActiveFocus()
            return
        }
        const ok = taskModel.updateTaskWithReminder(taskId, titleField.text, notesArea.text,
                                                    listField.text, priorityBox.currentIndex,
                                                    due.value, reminder.value)
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

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                Label { text: "Reminder date" }
                TextField {
                    id: reminderDateField
                    objectName: "editorReminderDate"
                    Layout.fillWidth: true
                    placeholderText: "YYYY-MM-DD (optional)"
                    selectByMouse: true
                    onAccepted: editor.save()
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: "Reminder time" }
                TextField {
                    id: reminderTimeField
                    objectName: "editorReminderTime"
                    Layout.fillWidth: true
                    placeholderText: "HH:MM (default 09:00)"
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
