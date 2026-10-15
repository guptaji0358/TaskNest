import QtQuick
import QtQuick.Controls
import "DateTimeInput.js" as DT

// Date input: type YYYY-MM-DD (also YYYY/MM/DD, YYYY.MM.DD) or open the
// calendar by clicking the field, its calendar button, or pressing
// Alt+Down / F4 (Down from an empty-handed field also opens it and moves the
// keyboard into the calendar). A valid entry is rewritten to YYYY-MM-DD when
// the field is left; an invalid one is left exactly as typed.
TNTextField {
    id: control

    property string label: "Date"        // names the field in messages
    property bool optional: true
    property bool externalInvalid: false // the owner reports an error for this field
    property bool touched: false

    readonly property var parsed: DT.parseDate(text, label)
    readonly property bool valid: parsed.empty === true || parsed.ok === true
    readonly property string problem: parsed.error ? parsed.error : ""
    readonly property alias calendarPopup: popup

    rightPadding: 44
    leftPadding: Theme.s3 // the calendar button is on the right, so no leading icon
    invalid: externalInvalid || (touched && !valid)
    placeholderText: "YYYY-MM-DD"
    inputMethodHints: Qt.ImhPreferNumbers

    onEditingFinished: {
        touched = true
        if (parsed.ok === true && text !== parsed.text)
            text = parsed.text // canonical form
    }
    onTextEdited: touched = false

    Accessible.name: label + " date"
    Accessible.description: "Format YYYY-MM-DD. Press Alt+Down to open the calendar."

    function openCalendar(takeFocus) {
        popup.open() // the calendar is only created while the popup is open
        const calendar = calendarLoader.item
        if (!calendar)
            return
        calendar.clearable = optional
        if (parsed.ok === true) {
            calendar.selectedYear = parsed.year
            calendar.selectedMonth = parsed.month
            calendar.selectedDay = parsed.day
        } else {
            calendar.selectedYear = 0
            calendar.selectedMonth = 0
            calendar.selectedDay = 0
        }
        calendar.reset()
        if (takeFocus)
            calendar.forceActiveFocus()
    }

    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_F4
                || (event.key === Qt.Key_Down && (event.modifiers & Qt.AltModifier))) {
            control.openCalendar(true)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape && popup.visible) {
            popup.close()
            event.accepted = true
        }
    }

    // Clicking into the field shows the calendar without taking the keyboard
    // from the text, so typing still works.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: control.openCalendar(false)
    }

    Item {
        id: openButton
        objectName: "dateOpenButton"
        width: 32
        height: 32
        anchors.right: parent.right
        anchors.rightMargin: Theme.s1
        anchors.verticalCenter: parent.verticalCenter
        Accessible.role: Accessible.Button
        Accessible.name: "Open calendar"
        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusSm
            color: buttonArea.containsMouse ? Theme.elevated : "transparent"
        }
        Icon { anchors.centerIn: parent; name: "today"; size: 16; color: Theme.textMuted }
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: control.openCalendar(true)
        }
    }

    Popup {
        id: popup
        objectName: "calendarPopup"
        y: control.height + Theme.s1
        padding: Theme.s3
        margins: Theme.s2 // stays inside the window (flips/moves when there is no room)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        background: Rectangle {
            radius: Theme.radiusMd
            color: Theme.elevated
            border.color: Theme.borderStrong
        }

        onClosed: control.touched = true

        contentItem: Loader {
            id: calendarLoader
            active: popup.visible
            focus: true
            sourceComponent: TNCalendar {
                focus: true
                onPicked: (year, month, day) => {
                    control.text = DT.formatDate(year, month, day)
                    control.touched = true
                    popup.close()
                    control.forceActiveFocus()
                }
                onCleared: {
                    control.clear()
                    control.touched = true
                    popup.close()
                    control.forceActiveFocus()
                }
                onCancelled: {
                    popup.close()
                    control.forceActiveFocus()
                }
            }
        }
    }
}
