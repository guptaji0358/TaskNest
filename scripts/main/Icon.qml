import QtQuick
import QtQuick.Shapes

// An extruded, lit icon drawn from Theme.icons (24x24 grid). No font, image or
// 3D-module assets: the depth is stacked vector copies of the same path (soft
// drop shadow, darker side walls, the colored front face and a light top-left
// edge), so icons still scale and recolor cleanly. Static; nothing animates.
Item {
    id: root

    property string name: ""
    property color color: Theme.text
    property real size: 18
    property real strokeWidth: 1.8
    // false draws the flat front face only.
    property bool depth: true

    readonly property string pathData: Theme.icons[name] !== undefined ? Theme.icons[name] : ""
    readonly property color sideColor: Qt.darker(color, Theme.iconSideDarken)
    readonly property color edgeColor: Qt.lighter(color, Theme.iconEdgeLighten)

    // Back to front: shadow, side walls, front face, lit edge. Offsets are in
    // 24-grid units; the front face sits at 0,0 and the depth recedes down-right.
    readonly property var layers: {
        const front = { dx: 0, dy: 0, color: color, width: strokeWidth };
        if (!depth)
            return [front];
        const n = Theme.iconDepthLayers;
        const s = Theme.iconDepthStep;
        const out = [{ dx: (n + 1) * s, dy: (n + 1) * s, color: Theme.shadow, width: strokeWidth + 0.6 }];
        for (let i = 0; i < n; ++i)
            out.push({ dx: (n - i) * s, dy: (n - i) * s,
                       color: Qt.lighter(sideColor, 1 + i * 0.12), width: strokeWidth });
        out.push(front);
        out.push({ dx: -strokeWidth * 0.22, dy: -strokeWidth * 0.22,
                   color: edgeColor, width: Math.max(0.5, strokeWidth * 0.32) });
        return out;
    }

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    Repeater {
        model: root.layers
        delegate: Shape {
            required property var modelData
            x: modelData.dx * root.size / 24
            y: modelData.dy * root.size / 24
            width: 24
            height: 24
            scale: root.size / 24
            transformOrigin: Item.TopLeft
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeColor: modelData.color
                strokeWidth: modelData.width
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: root.pathData }
            }
        }
    }
}
