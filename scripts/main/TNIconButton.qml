import QtQuick
import QtQuick.Controls

// Square icon-only button; `tip` is both its tooltip and accessible name.
Button {
    id: control

    property string iconName: ""
    property string tip: ""
    property bool danger: false

    implicitWidth: 32
    implicitHeight: 32
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: tip
    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 600

    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: control.iconName
            size: 16
            color: !control.enabled ? Theme.textFaint
                   : control.danger && (control.hovered || control.visualFocus) ? Theme.danger
                   : control.hovered ? Theme.text : Theme.textMuted
        }
    }

    background: Rectangle {
        radius: Theme.radiusSm
        color: control.down ? Theme.elevated
               : control.hovered ? (control.danger ? Theme.dangerSoft : Theme.surfaceRaised)
               : "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.focus
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
    }
}
