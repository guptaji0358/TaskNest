import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Optional details for the "add a task" row: priority, list, due date/time,
// reminder date/time and a one-line note. Everything is optional; the task
// title stays in the add field so "type a title, press Enter" still works.
Item {
    id: root

    property bool expanded: false
    property string errorText: ""

    // Pressed Enter in one of the fields.
    signal submitted()

    readonly property bool hasDetails: priorityBox.currentIndex !== 0
                                       || listField.text.trim() !== ""
                                       || dateField.text.trim() !== ""
                                       || timeField.text.trim() !== ""
                                       || reminderDateField.text.trim() !== ""
                                       || reminderTimeField.text.trim() !== ""
                                       || notesField.text.trim() !== ""

    visible: expanded
    implicitHeight: expanded ? column.implicitHeight : 0
    clip: true

    function reset() {
        priorityBox.currentIndex = 0
        listField.text = ""
        dateField.text = ""
        timeField.text = ""
        reminderDateField.text = ""
        reminderTimeField.text = ""
        notesField.text = ""
        errorText = ""
    }

    function focusFirst() {
        priorityBox.forceActiveFocus()
    }

    // { error } or { notes, list, priority, due, reminder } (invalid Date = none)
    function collect() {
        const due = Theme.parseDateTime(dateField.text, timeField.text, "Due", 23, 59)
        if (due.error) {
            errorText = due.error
            dateField.forceActiveFocus()
            return { error: due.error }
        }
        const reminder = Theme.parseDateTime(reminderDateField.text, reminderTimeField.text,
                                             "Reminder", 9, 0)
        if (reminder.error) {
            errorText = reminder.error
            reminderDateField.forceActiveFocus()
            return { error: reminder.error }
        }
        errorText = ""
        return {
            notes: notesField.text,
            list: listField.text,
            priority: priorityBox.currentIndex,
            due: due.value,
            reminder: reminder.value
        }
    }

    ColumnLayout {
        id: column
        width: parent.width
        spacing: Theme.s2

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            spacing: Theme.s2

            TNComboBox {
                id: priorityBox
                objectName: "quickPriority"
                Layout.preferredWidth: 150
                model: ["No priority", "Medium", "High"]
                Accessible.name: "Priority"
            }
            TNTextField {
                id: listField
                objectName: "quickList"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                iconName: "list"
                placeholderText: "List (default Inbox)"
                onAccepted: root.submitted()
            }
            TNDateField {
                id: dateField
                objectName: "quickDate"
                label: "Due"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                placeholderText: "Due YYYY-MM-DD"
                externalInvalid: root.errorText.indexOf("Due") === 0 || root.errorText.indexOf("That due") === 0
                onAccepted: root.submitted()
            }
            TNTimeField {
                id: timeField
                objectName: "quickTime"
                label: "Due"
                defaultHour: 23
                defaultMinute: 59
                Layout.preferredWidth: 120
                placeholderText: "HH:mm"
                onAccepted: root.submitted()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            spacing: Theme.s2

            TNDateField {
                id: reminderDateField
                objectName: "quickReminderDate"
                label: "Reminder"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                placeholderText: "Remind YYYY-MM-DD"
                externalInvalid: root.errorText.indexOf("Reminder") === 0 || root.errorText.indexOf("That reminder") === 0
                onAccepted: root.submitted()
            }
            TNTimeField {
                id: reminderTimeField
                objectName: "quickReminderTime"
                label: "Reminder"
                defaultHour: 9
                defaultMinute: 0
                Layout.preferredWidth: 120
                placeholderText: "HH:mm"
                onAccepted: root.submitted()
            }
            TNTextField {
                id: notesField
                objectName: "quickNotes"
                Layout.fillWidth: true
                Layout.preferredWidth: 2
                placeholderText: "Note (one line; use Edit for more)"
                onAccepted: root.submitted()
            }
        }

        TNLabel {
            objectName: "quickAddError"
            Layout.fillWidth: true
            visible: root.errorText !== ""
            text: root.errorText
            role: "caption"
            color: Theme.danger
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
    }
}
