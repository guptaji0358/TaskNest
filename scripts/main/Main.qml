import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    width: 900
    height: 600
    minimumWidth: 640
    minimumHeight: 400
    visible: true
    title: "TaskNest"

    ColumnLayout {
        anchors.centerIn: parent
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
    }
}
