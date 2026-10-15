import QtQuick
import QtQuick.Controls

// Editable two-digit number box with wrap-around (hours, minutes, seconds).
// Typing is validated against from/to; out-of-range input is rejected.
SpinBox {
    id: control

    editable: true
    wrap: true
    implicitHeight: Theme.controlHeight
    implicitWidth: 92
    focusPolicy: Qt.StrongFocus
    leftPadding: Theme.s3
    rightPadding: 28

    textFromValue: (value) => (value < 10 ? "0" : "") + value
    valueFromText: (text) => Number(text)
    validator: IntValidator { bottom: control.from; top: control.to }

    contentItem: TextInput {
        text: control.displayText
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontBody
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        selectByMouse: true
    }

    background: Rectangle {
        radius: Theme.radiusMd
        color: Theme.surfaceRaised
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.focus : Theme.border
    }

    up.indicator: Item {
        x: control.width - width - 2
        y: 2
        width: 24
        height: control.height / 2 - 2
        Icon { anchors.centerIn: parent; name: "chevron"; size: 11; rotation: 180
               color: control.up.hovered ? Theme.text : Theme.textMuted }
    }
    down.indicator: Item {
        x: control.width - width - 2
        y: control.height / 2
        width: 24
        height: control.height / 2 - 2
        Icon { anchors.centerIn: parent; name: "chevron"; size: 11
               color: control.down.hovered ? Theme.text : Theme.textMuted }
    }
}
