import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    // Set from main.cpp before the window is created.
    required property string databaseError
    required property string databasePath
    required property var taskModel // null when the database is unavailable

    readonly property bool databaseOk: databaseError.length === 0

    width: 900
    height: 600
    minimumWidth: 640
    minimumHeight: 400
    visible: true
    title: "TaskNest"

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 640)
        spacing: 12

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "TaskNest"
            font.pixelSize: 48
            font.bold: true
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "Your tasks. Your computer. Your control."
            font.pixelSize: 18
            opacity: 0.7
        }

        // Read-only check that the task model loads; the real task UI comes later.
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            Layout.topMargin: 16
            visible: root.databaseOk
            clip: true
            model: root.taskModel
            header: Label {
                text: (root.taskModel ? root.taskModel.count : 0) + " task(s)"
                opacity: 0.7
            }
            delegate: Label {
                required property string title
                required property bool completed
                text: (completed ? "☑ " : "☐ ") + title
                font.strikeout: completed
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            Layout.topMargin: 24
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: root.databaseOk ? palette.windowText : "#c62828"
            opacity: root.databaseOk ? 0.5 : 1
            text: root.databaseOk
                  ? "Database ready: " + root.databasePath
                  : "Database unavailable. Your data has not been modified.\n" + root.databaseError
        }
    }
}
