import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A short-lived notice with one optional action (used for "Undo").
// It hides itself with a single one-shot timer; nothing polls.
Rectangle {
    id: toast

    property string message: ""
    property string actionText: ""
    property string iconName: "trash"
    property int duration: 8000

    signal actionTriggered()
    signal expired()   // the time ran out (not: the action was used)

    visible: false
    opacity: 0
    implicitWidth: row.implicitWidth + Theme.s4 * 2
    implicitHeight: 52
    radius: Theme.radiusMd
    color: Theme.elevated
    border.width: 1
    border.color: Theme.borderStrong

    Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

    function show(text, action, ms, icon) {
        message = text
        actionText = action || ""
        iconName = icon || "trash"
        if (ms !== undefined)
            duration = ms
        visible = true
        opacity = 1
        timer.restart()
    }

    function hide() {
        timer.stop()
        opacity = 0
        visible = false
    }

    Timer {
        id: timer
        interval: toast.duration
        repeat: false
        onTriggered: {
            toast.hide()
            toast.expired()
        }
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Theme.s4
        anchors.rightMargin: Theme.s2
        spacing: Theme.s3

        Icon {
            name: toast.iconName
            size: 16
            color: Theme.textMuted
            Layout.alignment: Qt.AlignVCenter
        }
        TNLabel {
            objectName: "toastText"
            Layout.fillWidth: true
            text: toast.message
            elide: Text.ElideRight
            Accessible.role: Accessible.AlertMessage
        }
        TNButton {
            objectName: "toastAction"
            visible: toast.actionText !== ""
            variant: "tonal"
            iconName: "undo"
            text: toast.actionText
            implicitHeight: 34
            onClicked: toast.actionTriggered()
        }
        TNIconButton {
            objectName: "toastClose"
            iconName: "close"
            tip: "Dismiss"
            onClicked: {
                toast.hide()
                toast.expired()
            }
        }
    }
}
