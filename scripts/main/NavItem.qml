import QtQuick
import QtQuick.Controls

// Sidebar entry. The active item gets a soft lime tint, a lime icon and a
// short "notch" on the leading edge. `collapsed` shows the icon only.
AbstractButton {
    id: control

    property string iconName: ""
    property string label: ""
    property bool active: false
    property bool collapsed: false

    text: label
    implicitHeight: 36
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    Accessible.name: label
    ToolTip.visible: collapsed && hovered
    ToolTip.text: label
    ToolTip.delay: 300

    background: Rectangle {
        radius: Theme.radiusSm
        color: control.active ? Theme.accentSoft
               : control.down ? Theme.layerHigh
               : control.hovered ? Theme.layerMid : "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.focus
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Rectangle {
            width: 3
            height: control.active ? 18 : 6
            radius: 2
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.accent
            opacity: control.active ? 1 : 0
            Behavior on height { NumberAnimation { duration: Theme.durationBase; easing.type: Theme.easeOut } }
            Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
        }
    }

    contentItem: Item {
        // In the icon rail, custom lists show their initial so they can be told apart.
        readonly property bool useInitial: control.collapsed && control.iconName === "list"

        TNLabel {
            visible: parent.useInitial
            anchors.centerIn: glyph
            text: control.label.charAt(0).toUpperCase()
            font.family: Theme.displayFamily
            font.pixelSize: 17
            font.weight: Font.DemiBold
            color: control.active ? Theme.accent : (control.hovered ? Theme.text : Theme.textMuted)
        }

        Icon {
            id: glyph
            visible: !parent.useInitial
            name: control.iconName
            size: 18
            color: control.active ? Theme.accent : (control.hovered ? Theme.text : Theme.textMuted)
            anchors.verticalCenter: parent.verticalCenter
            x: control.collapsed ? (parent.width - width) / 2 : Theme.s3 + 2
        }
        TNLabel {
            visible: !control.collapsed
            text: control.label
            elide: Text.ElideRight
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: glyph.right
            anchors.leftMargin: Theme.s3
            anchors.right: parent.right
            anchors.rightMargin: Theme.s2
            color: control.active ? Theme.text : (control.hovered ? Theme.text : Theme.textMuted)
            font.weight: control.active ? Font.DemiBold : Font.Medium
        }
    }
}
