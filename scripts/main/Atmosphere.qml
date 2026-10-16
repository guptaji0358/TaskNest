import QtQuick
import QtQuick.Shapes

// Soft colored light behind the workspace: a lime glow at the upper left and a
// violet one at the lower right. Purely decorative (never takes input), kept at
// low alpha so task text always has full contrast, and static: nothing here
// animates or repaints after the first frame.
Item {
    id: root
    enabled: false

    Shape {
        anchors.fill: parent

        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: root.width * 0.12
                centerY: -root.height * 0.05
                centerRadius: Math.max(root.width, root.height) * 0.55
                focalX: centerX
                focalY: centerY
                GradientStop { position: 0.0; color: Theme.glowAccent }
                GradientStop { position: 1.0; color: "transparent" }
            }
            startX: 0; startY: 0
            PathLine { x: root.width; y: 0 }
            PathLine { x: root.width; y: root.height }
            PathLine { x: 0; y: root.height }
            PathLine { x: 0; y: 0 }
        }
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: root.width * 0.98
                centerY: root.height * 1.02
                centerRadius: Math.max(root.width, root.height) * 0.6
                focalX: centerX
                focalY: centerY
                GradientStop { position: 0.0; color: Theme.glowSecondary }
                GradientStop { position: 1.0; color: "transparent" }
            }
            startX: 0; startY: 0
            PathLine { x: root.width; y: 0 }
            PathLine { x: root.width; y: root.height }
            PathLine { x: 0; y: root.height }
            PathLine { x: 0; y: 0 }
        }
    }
}
