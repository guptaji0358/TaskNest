import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes

// "An egg in the nest": two nested bowls with an accent token resting in
// them. The same motif as the logo, so an empty list feels intentional.
Item {
    id: root

    property string title: ""
    property string text: ""

    implicitWidth: 360
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width
        spacing: Theme.s3

        Item {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 120
            implicitHeight: 104

            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer

                ShapePath {
                    strokeColor: Theme.borderStrong
                    strokeWidth: 2
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    PathSvg { path: "M8 44a52 52 0 0 0 104 0" }
                }
                ShapePath {
                    strokeColor: Theme.border
                    strokeWidth: 2
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    PathSvg { path: "M24 44a36 36 0 0 0 72 0" }
                }
            }

            Rectangle {
                width: 30
                height: 30
                radius: 15
                x: 45
                y: 24
                color: Theme.accent
                Icon {
                    anchors.centerIn: parent
                    name: "check"
                    size: 18
                    strokeWidth: 2.4
                    color: Theme.accentText
                }
            }
        }

        TNLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            role: "heading"
            text: root.title
            visible: root.title !== ""
        }
        TNLabel {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: Theme.textMuted
            text: root.text
        }
    }
}
