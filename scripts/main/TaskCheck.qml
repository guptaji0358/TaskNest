import QtQuick
import QtQuick.Controls

// Completion control: a ring that fills with the accent and draws a tick.
CheckBox {
    id: control

    padding: 4
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    implicitWidth: 30
    implicitHeight: 30

    indicator: Item {
        x: (control.width - width) / 2
        y: (control.height - height) / 2
        width: 22
        height: 22

        Rectangle {
            id: ring
            anchors.fill: parent
            radius: width / 2
            color: control.checked ? Theme.accent
                   : control.down ? Theme.surfaceRaised : "transparent"
            border.width: control.visualFocus ? 2 : 1.5
            border.color: control.visualFocus ? Theme.focus
                          : control.checked ? Theme.accent
                          : control.hovered ? Theme.accent : Theme.borderStrong
            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
        }

        Icon {
            anchors.centerIn: parent
            name: "check"
            size: 14
            strokeWidth: 2.6
            color: Theme.accentText
            opacity: control.checked ? 1 : 0
            scale: control.checked ? 1 : 0.4
            Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
            Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutBack } }
        }
    }

    contentItem: Item {}
}
