import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Modal editor for one task. Nothing is written until Save succeeds;
// Cancel / Escape leave the stored task untouched.
TNDialog {
    id: editor

    required property var taskModel

    property var taskId: 0
    property string errorText: ""
    property bool closing: false // true from the moment the dialog starts to hide

    onAboutToShow: closing = false
    onAboutToHide: closing = true

    title: "Edit task"
    width: Math.min(560, parent ? parent.width - 32 : 560)
    height: Math.min(implicitHeight, parent ? parent.height - 32 : implicitHeight)

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
        titleField.cursorPosition = 0 // long titles open scrolled to their start
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
        if (closing || !visible)
            return // a second click/Enter while the dialog is closing must not save twice
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

    contentItem: ScrollView {
        id: scroller
        contentWidth: availableWidth
        clip: true
        implicitHeight: form.implicitHeight
        ScrollBar.vertical: TNScrollBar {}

        ColumnLayout {
            id: form
            width: scroller.availableWidth
            spacing: Theme.s3

            TNTextField {
                id: titleField
                objectName: "editorTitle"
                Layout.fillWidth: true
                look: "title"
                placeholderText: "What needs doing?"
                onAccepted: editor.save()
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                TNLabel { role: "overline"; text: "Notes" }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 84
                    radius: Theme.radiusMd
                    color: Theme.surfaceRaised
                    border.width: notesArea.activeFocus ? 2 : 1
                    border.color: notesArea.activeFocus ? Theme.focus : Theme.border

                    ScrollView {
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true

                        TextArea {
                            id: notesArea
                            objectName: "editorNotes"
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            color: Theme.text
                            placeholderText: "Add details…"
                            placeholderTextColor: Theme.textFaint
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.accentText
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontBody
                            // Enter keeps typing multi-line notes; Ctrl+Enter saves.
                            Keys.onPressed: (event) => {
                                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                        && (event.modifiers & Qt.ControlModifier)) {
                                    event.accepted = true
                                    editor.save()
                                }
                            }
                            background: null
                            leftPadding: Theme.s3
                            rightPadding: Theme.s3
                            topPadding: Theme.s2
                            bottomPadding: Theme.s2
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: false
                spacing: Theme.s3

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    spacing: 6
                    TNLabel { role: "overline"; text: "Priority" }
                    TNComboBox {
                        id: priorityBox
                        objectName: "editorPriority"
                        Layout.fillWidth: true
                        model: ["None", "Medium", "High"]
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    spacing: 6
                    TNLabel { role: "overline"; text: "List" }
                    TNTextField {
                        id: listField
                        objectName: "editorList"
                        Layout.fillWidth: true
                        placeholderText: "Inbox"
                        onAccepted: editor.save()
                    }
                }
            }

            // Due
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                RowLayout {
                    spacing: 6
                    Icon { name: "today"; size: 14; color: Theme.textFaint }
                    TNLabel { role: "overline"; text: "Due" }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    spacing: Theme.s3

                    TNTextField {
                        id: dateField
                        objectName: "editorDate"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 3
                        placeholderText: "YYYY-MM-DD"
                        onAccepted: editor.save()
                    }
                    TNTextField {
                        id: timeField
                        objectName: "editorTime"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 2
                        placeholderText: "HH:MM (optional)"
                        onAccepted: editor.save()
                    }
                }
            }

            // Reminder
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                RowLayout {
                    spacing: 6
                    Icon { name: "bell"; size: 14; color: Theme.secondary }
                    TNLabel { role: "overline"; text: "Reminder"; color: Theme.secondary }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    spacing: Theme.s3

                    TNTextField {
                        id: reminderDateField
                        objectName: "editorReminderDate"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 3
                        placeholderText: "YYYY-MM-DD (optional)"
                        onAccepted: editor.save()
                    }
                    TNTextField {
                        id: reminderTimeField
                        objectName: "editorReminderTime"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 2
                        placeholderText: "HH:MM (default 09:00)"
                        onAccepted: editor.save()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                visible: editor.errorText !== ""
                implicitHeight: errorRow.implicitHeight + 16
                radius: Theme.radiusMd
                color: Theme.dangerSoft
                border.color: Theme.dangerLine

                RowLayout {
                    id: errorRow
                    anchors.fill: parent
                    anchors.margins: 8
                    anchors.leftMargin: Theme.s3
                    spacing: Theme.s2

                    Icon { name: "warning"; size: 16; color: Theme.danger; Layout.alignment: Qt.AlignTop }
                    TNLabel {
                        objectName: "editorError"
                        Layout.fillWidth: true
                        visible: editor.errorText !== ""
                        text: editor.errorText
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                }
            }
        }
    }

    footer: Item {
        implicitHeight: 68

        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.s5
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.s2

            TNButton {
                objectName: "editorCancel"
                variant: "ghost"
                text: "Cancel"
                onClicked: editor.close()
            }
            TNButton {
                objectName: "editorSave"
                variant: "primary"
                iconName: "check"
                text: "Save"
                onClicked: editor.save()
            }
        }
    }
}
