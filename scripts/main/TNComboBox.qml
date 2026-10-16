import QtQuick
import QtQuick.Controls

// Drop-down list in the TaskNest style. Same API as ComboBox.
ComboBox {
    id: control

    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    leftPadding: Theme.s3
    rightPadding: 36
    topPadding: 0
    bottomPadding: 0

    delegate: ItemDelegate {
        id: item

        required property var modelData
        required property int index

        width: control.width - 8
        height: 34
        padding: 0
        leftPadding: Theme.s3
        highlighted: control.highlightedIndex === index
        hoverEnabled: true

        readonly property bool isCurrent: item.index === control.currentIndex

        contentItem: Item {
            TNLabel {
                anchors.left: parent.left
                anchors.right: tick.left
                anchors.verticalCenter: parent.verticalCenter
                text: item.modelData
                elide: Text.ElideRight
                color: item.isCurrent ? Theme.accent : Theme.text
                font.weight: item.isCurrent ? Font.DemiBold : Font.Normal
            }
            // The selected item is marked by a tick as well as by color.
            Icon {
                id: tick
                visible: item.isCurrent
                name: "check"
                size: 14
                strokeWidth: 2.4
                color: Theme.accent
                anchors.right: parent.right
                anchors.rightMargin: Theme.s3
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        background: Rectangle {
            radius: Theme.radiusSm
            color: item.down ? Theme.layerHigh
                   : item.highlighted || item.hovered ? Theme.layerMid
                   : item.isCurrent ? Theme.accentSoft : "transparent"
            border.width: item.highlighted && control.popup.visible && item.visualFocus ? 1 : 0
            border.color: Theme.focus
        }
    }

    indicator: Icon {
        name: "chevron"
        size: 14
        color: control.enabled ? Theme.textMuted : Theme.textFaint
        x: control.width - width - Theme.s3
        y: (control.height - height) / 2
        rotation: control.popup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: Theme.durationFast } }
    }

    contentItem: TNLabel {
        text: control.displayText
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        color: control.enabled ? Theme.text : Theme.textFaint
    }

    background: Rectangle {
        radius: Theme.radiusMd
        color: Theme.surfaceRaised
        border.width: control.visualFocus || control.popup.visible ? 2 : 1
        border.color: !control.enabled ? Theme.border
                      : control.visualFocus || control.popup.visible ? Theme.focus
                      : control.hovered ? Theme.borderStrong : Theme.border
        opacity: control.enabled ? 1 : 0.55
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
    }

    popup: Popup {
        y: control.height + Theme.s1
        width: control.width
        margins: Theme.s2 // flips above / shifts when there is no room below
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 260)
        padding: 4

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }
        background: TNPopupSurface {}
    }
}
