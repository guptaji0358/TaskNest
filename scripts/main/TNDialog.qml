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

    // Depth: stacked translucent layers form a soft shadow below the panel
    // (no effects module needed), then the panel with a lit top edge.
    background: Item {
        Repeater {
            model: 4
            Rectangle {
                required property int index
                anchors.fill: parent
                anchors.margins: -(index + 1) * 5
                anchors.topMargin: -(index + 1) * 3
                anchors.bottomMargin: -(index + 1) * 7
                radius: Theme.radiusLg + (index + 1) * 5
                color: Qt.rgba(0, 0, 0, 0.13)
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusLg
            color: Theme.elevated
            border.width: 1
            border.color: Theme.borderStrong
            Rectangle {
                height: 1
                anchors.top: parent.top
                anchors.topMargin: 1
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.radiusLg
                anchors.rightMargin: Theme.radiusLg
                color: Theme.highlightStrong
            }
        }
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
