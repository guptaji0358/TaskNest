import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "DateTimeInput.js" as DT

// Time input, 24-hour: type HH:mm, HH:mm:ss, or with hyphens (HH-mm,
// HH-mm-ss), or open the picker (click the clock button, Alt+Down / F4).
// Leaving the field rewrites a valid entry to HH:mm (HH:mm:ss only when the
// seconds are not zero); an invalid entry is left exactly as typed.
//
// The field stays blank until the user sets a time: the owner's default
// (placeholder) applies to a blank field. The picker starts at that default.
TNTextField {
    id: control

    property string label: "Time"
    property int defaultHour: 9
    property int defaultMinute: 0
    property bool showSeconds: false     // seconds box in the picker
    property bool externalInvalid: false
    property bool touched: false

    readonly property var parsed: DT.parseTime(text, label)
    readonly property bool valid: parsed.empty === true || parsed.ok === true
    readonly property string problem: parsed.error ? parsed.error : ""
    readonly property alias timePopup: popup

    rightPadding: 44
    invalid: externalInvalid || (touched && !valid)
    placeholderText: "HH:mm"
    inputMethodHints: Qt.ImhPreferNumbers

    onEditingFinished: {
        touched = true
        if (parsed.ok === true && text !== parsed.text)
            text = parsed.text
    }
    onTextEdited: touched = false

    Accessible.name: label + " time"
    Accessible.description: "24-hour time, HH:mm. Press Alt+Down to open the time picker."

    function openPicker(takeFocus) {
        popup.open() // the picker content is only created while the popup is open
        const picker = pickerLoader.item
        if (!picker)
            return
        if (parsed.ok === true) {
            picker.hourBox.value = parsed.hour
            picker.minuteBox.value = parsed.minute
            picker.secondBox.value = parsed.second
        } else {
            picker.hourBox.value = defaultHour
            picker.minuteBox.value = defaultMinute
            picker.secondBox.value = 0
        }
        if (takeFocus)
            picker.hourBox.forceActiveFocus()
    }

    // The spin boxes commit typed text on focus loss; read it directly so
    // "type 7, press Enter" uses the 7.
    function typed(box) {
        const n = Number(box.contentItem.text)
        return Number.isInteger(n) && n >= box.from && n <= box.to ? n : box.value
    }

    function applyPicked() {
        const picker = pickerLoader.item
        if (!picker)
            return
        control.text = DT.formatTime(typed(picker.hourBox), typed(picker.minuteBox),
                                     picker.secondBox.visible ? typed(picker.secondBox) : 0)
        control.touched = true
        popup.close()
        control.forceActiveFocus()
    }

    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_F4
                || (event.key === Qt.Key_Down && (event.modifiers & Qt.AltModifier))) {
            control.openPicker(true)
            event.accepted = true
        }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: control.openPicker(false)
    }

    Item {
        objectName: "timeOpenButton"
        width: 32
        height: 32
        anchors.right: parent.right
        anchors.rightMargin: Theme.s1
        anchors.verticalCenter: parent.verticalCenter
        Accessible.role: Accessible.Button
        Accessible.name: "Open time picker"
        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusSm
            color: buttonArea.containsMouse ? Theme.elevated : "transparent"
        }
        Icon { anchors.centerIn: parent; name: "clock"; size: 16; color: Theme.textMuted }
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: control.openPicker(true)
        }
    }

    Popup {
        id: popup
        objectName: "timePopup"
        y: control.height + Theme.s1
        padding: Theme.s3
        margins: Theme.s2
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        background: TNPopupSurface {}
        onClosed: control.touched = true

        contentItem: Loader {
            id: pickerLoader
            active: popup.visible
            sourceComponent: pickerComponent
        }
    }

    Component {
        id: pickerComponent
        ColumnLayout {
            property alias hourBox: hourBox
            property alias minuteBox: minuteBox
            property alias secondBox: secondBox
            spacing: Theme.s3

            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Escape) {
                    popup.close()
                    control.forceActiveFocus()
                    event.accepted = true
                }
            }

            RowLayout {
                spacing: Theme.s2

                TNSpinBox {
                    id: hourBox
                    objectName: "timeHour"
                    from: 0
                    to: 23
                    Accessible.name: "Hour"
                    Keys.onReturnPressed: control.applyPicked()
                }
                TNLabel { role: "heading"; text: ":" }
                TNSpinBox {
                    id: minuteBox
                    objectName: "timeMinute"
                    from: 0
                    to: 59
                    Accessible.name: "Minute"
                    Keys.onReturnPressed: control.applyPicked()
                }
                TNLabel { visible: secondBox.visible; role: "heading"; text: ":" }
                TNSpinBox {
                    id: secondBox
                    objectName: "timeSecond"
                    visible: control.showSeconds || control.parsed.second > 0
                    from: 0
                    to: 59
                    Accessible.name: "Second"
                    Keys.onReturnPressed: control.applyPicked()
                }
            }

            Flow {
                Layout.fillWidth: true
                Layout.preferredWidth: 260
                spacing: Theme.s1
                Repeater {
                    model: [[8, 0], [9, 0], [12, 0], [17, 0], [20, 0]]
                    delegate: TNButton {
                        required property var modelData
                        variant: "ghost"
                        text: DT.formatTime(modelData[0], modelData[1], 0)
                        implicitHeight: 30
                        focusPolicy: Qt.NoFocus
                        onClicked: {
                            hourBox.value = modelData[0]
                            minuteBox.value = modelData[1]
                            secondBox.value = 0
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2
                TNButton {
                    objectName: "timeClear"
                    variant: "ghost"
                    text: "Clear"
                    implicitHeight: 32
                    focusPolicy: Qt.NoFocus
                    onClicked: {
                        control.clear()
                        control.touched = true
                        popup.close()
                        control.forceActiveFocus()
                    }
                }
                Item { Layout.fillWidth: true }
                TNButton {
                    objectName: "timeCancel"
                    variant: "ghost"
                    text: "Cancel"
                    implicitHeight: 32
                    focusPolicy: Qt.NoFocus
                    onClicked: { popup.close(); control.forceActiveFocus() }
                }
                TNButton {
                    objectName: "timeSet"
                    variant: "primary"
                    text: "Set"
                    implicitHeight: 32
                    focusPolicy: Qt.NoFocus
                    onClicked: control.applyPicked()
                }
            }
        }
    }
}
