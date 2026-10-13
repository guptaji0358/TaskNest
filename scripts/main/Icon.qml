import QtQuick
import QtQuick.Shapes

// A stroked line icon drawn from Theme.icons (24x24 grid). No font or image
// assets: icons are vector geometry, so they scale and recolor cleanly.
Item {
    id: root

    property string name: ""
    property color color: Theme.text
    property real size: 18
    property real strokeWidth: 1.8

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    Shape {
        width: 24
        height: 24
        scale: root.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: root.color
            strokeWidth: root.strokeWidth
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathSvg {
                path: Theme.icons[root.name] !== undefined ? Theme.icons[root.name] : ""
            }
        }
    }
}
