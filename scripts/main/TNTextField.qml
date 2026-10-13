import QtQuick
import QtQuick.Controls

// Single-line input. Optional leading icon; `clearable` adds a clear button.
TextField {
    id: control

    property string iconName: ""
    property bool clearable: false
    property bool invalid: false // danger outline (validation message shown by the caller)
    // "field" (boxed) | "title" (larger, for the task title)
    property string look: "field"

    readonly property bool _large: look === "title"

    implicitHeight: _large ? 48 : Theme.controlHeight
    leftPadding: iconName !== "" ? 40 : Theme.s3
    rightPadding: clearable && text.length > 0 ? 36 : Theme.s3
    topPadding: 0
    bottomPadding: 0
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true
    hoverEnabled: true

    color: Theme.text
    placeholderTextColor: Theme.textFaint
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    font.family: _large ? Theme.displayFamily : Theme.fontFamily
    font.pixelSize: _large ? Theme.fontHeading : Theme.fontBody
    font.weight: _large ? Font.DemiBold : Font.Normal

    background: Rectangle {
        radius: Theme.radiusMd
        color: control.activeFocus ? Theme.surfaceRaised : (control.hovered ? Theme.surfaceHover : Theme.surfaceRaised)
        border.width: control.activeFocus ? 2 : 1
        border.color: !control.enabled ? Theme.border
                      : control.invalid ? Theme.danger
                      : control.activeFocus ? Theme.focus
                      : control.hovered ? Theme.borderStrong : Theme.border
        opacity: control.enabled ? 1 : 0.55
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

        Icon {
            visible: control.iconName !== ""
            name: control.iconName
            size: 16
            x: Theme.s3
            anchors.verticalCenter: parent.verticalCenter
            color: control.activeFocus ? Theme.accent : Theme.textFaint
        }

        Item {
            id: clearButton
            visible: control.clearable && control.text.length > 0
            width: 24
            height: 24
            anchors.right: parent.right
            anchors.rightMargin: Theme.s2
            anchors.verticalCenter: parent.verticalCenter

            Rectangle {
                anchors.fill: parent
                radius: 12
                color: clearArea.containsMouse ? Theme.elevated : "transparent"
            }
            Icon { anchors.centerIn: parent; name: "close"; size: 13; color: Theme.textMuted }
            MouseArea {
                id: clearArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    control.clear()
                    control.forceActiveFocus()
                }
            }
        }
    }
}
