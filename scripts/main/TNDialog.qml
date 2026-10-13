import QtQuick
import QtQuick.Controls

// Modal dialog frame shared by the task editor and the delete confirmation:
// elevated surface, large radius, quiet scrim, short fade.
Dialog {
    id: control

    modal: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape
    padding: Theme.s5
    topPadding: Theme.s2
    bottomPadding: Theme.s4

    Overlay.modal: Rectangle { color: Theme.scrim }

    background: Rectangle {
        radius: Theme.radiusLg
        color: Theme.elevated
        border.width: 1
        border.color: Theme.borderStrong
    }

    header: Item {
        implicitHeight: 60

        TNLabel {
            role: "heading"
            text: control.title
            elide: Text.ElideRight
            anchors.left: parent.left
            anchors.leftMargin: Theme.s5
            anchors.right: parent.right
            anchors.rightMargin: Theme.s5
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: 4
        }
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durationFast }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durationFast }
    }
}
