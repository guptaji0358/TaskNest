import QtQuick
import QtQuick.Controls

ScrollBar {
    id: control

    implicitWidth: 10
    padding: 2
    hoverEnabled: true

    contentItem: Rectangle {
        implicitWidth: 6
        radius: 3
        visible: control.size < 1.0 // nothing to scroll: no thumb
        color: control.pressed ? Theme.textMuted : Theme.borderStrong
        opacity: control.active || control.hovered ? 1 : 0.55
        Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
    }
}
