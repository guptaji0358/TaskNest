import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// In-app reminder: violet (the "reminder" color), visible but not modal.
// Complete is a positive tonal action; Dismiss is deliberately quiet.
Rectangle {
    id: banner

    property string title: ""
    property var when: null

    signal completeClicked()
    signal dismissClicked()

    implicitHeight: 60
    radius: Theme.radiusMd
    color: Theme.secondarySoft
    border.width: 1
    border.color: Theme.secondaryLine

    // Arrival: fades in while sliding down a few pixels (instant with Reduce motion).
    transform: Translate { id: slide }
    ParallelAnimation {
        running: true
        NumberAnimation { target: banner; property: "opacity"; from: 0; to: 1
                          duration: Theme.durationBase; easing.type: Theme.easeOut }
        NumberAnimation { target: slide; property: "y"; from: -Theme.enterOffset; to: 0
                          duration: Theme.durationBase; easing.type: Theme.easeOut }
    }

    Rectangle {
        height: 1
        anchors.top: parent.top
        anchors.topMargin: 1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.radiusMd
        anchors.rightMargin: Theme.radiusMd
        color: Theme.secondaryLine
    }

    Rectangle {
        width: 4
        radius: 2
        color: Theme.secondary
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 8
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 22
        anchors.rightMargin: Theme.s3
        spacing: Theme.s3

        Icon {
            name: "bell"
            size: 20
            color: Theme.secondary
            Layout.alignment: Qt.AlignVCenter
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            TNLabel {
                role: "overline"
                text: "Reminder"
                color: Theme.secondary
            }
            TNLabel {
                objectName: "reminderText"
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.weight: Font.Medium
                text: banner.title + (banner.when
                                      ? "  ·  " + Qt.formatDateTime(banner.when, "ddd d MMM, HH:mm") : "")
                Accessible.role: Accessible.AlertMessage
            }
        }

        TNButton {
            objectName: "reminderComplete"
            variant: "success"
            iconName: "check"
            text: "Complete"
            onClicked: banner.completeClicked()
        }
        TNButton {
            objectName: "reminderDismiss"
            variant: "ghost"
            text: "Dismiss"
            onClicked: banner.dismissClicked()
        }
    }
}
