import QtQuick

// Background for popups (calendar, time, drop-downs): an elevated plane with a
// soft stacked shadow beneath and a lit top edge, matching dialogs. Used as
// `background: TNPopupSurface {}`.
Item {
    // The shadow extends past the item, so the popup's own margins keep it on screen.
    Repeater {
        model: 3
        Rectangle {
            required property int index
            anchors.fill: parent
            anchors.margins: -(index + 1) * 3
            anchors.topMargin: -(index + 1) * 2
            anchors.bottomMargin: -(index + 1) * 5
            radius: Theme.radiusMd + (index + 1) * 3
            color: Qt.rgba(0, 0, 0, 0.12)
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMd
        color: Theme.elevated
        border.width: 1
        border.color: Theme.borderStrong

        Rectangle {
            height: 1
            anchors.top: parent.top
            anchors.topMargin: 1
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.radiusMd
            anchors.rightMargin: Theme.radiusMd
            color: Theme.highlightStrong
        }
    }
}
