import QtQuick
import QtQuick.Controls

// Text button with an optional leading icon.
// variant: primary (lime) | secondary | ghost | success | danger
Button {
    id: control

    property string variant: "secondary"
    property string iconName: ""

    readonly property bool _filled: variant === "primary"
    readonly property color _fg: {
        if (!enabled)
            return Theme.textFaint
        switch (variant) {
        case "primary": return Theme.accentText
        case "success": return Theme.success
        case "danger": return Theme.danger
        case "ghost": return Theme.textMuted
        default: return Theme.text
        }
    }

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(implicitHeight, row.implicitWidth + leftPadding + rightPadding)
    leftPadding: Theme.s4
    rightPadding: Theme.s4
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row
            anchors.centerIn: parent
            spacing: Theme.s2

            Icon {
                visible: control.iconName !== ""
                name: control.iconName
                color: control._fg
                size: 16
                anchors.verticalCenter: parent.verticalCenter
            }
            TNLabel {
                visible: control.text !== ""
                text: control.text
                color: control._fg
                font.weight: control._filled ? Font.DemiBold : Font.Medium
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    background: Rectangle {
        radius: Theme.radiusSm
        border.width: control.visualFocus ? 2 : (control.variant === "secondary"
                                                 || control.variant === "success"
                                                 || control.variant === "danger" ? 1 : 0)
        border.color: {
            if (control.visualFocus)
                return Theme.focus
            switch (control.variant) {
            case "success": return Theme.successLine
            case "danger": return Theme.dangerLine
            default: return Theme.borderStrong
            }
        }
        color: {
            if (!control.enabled)
                return control.variant === "ghost" ? "transparent" : Theme.surfaceRaised
            const down = control.down
            const over = control.hovered
            switch (control.variant) {
            case "primary":
                return down ? Theme.accentPressed : over ? Theme.accentHover : Theme.accent
            case "success":
                return down || over ? Theme.successLine : Theme.successSoft
            case "danger":
                return down || over ? Theme.dangerLine : Theme.dangerSoft
            case "ghost":
                return down ? Theme.surfaceRaised : over ? Theme.surfaceHover : "transparent"
            default:
                return down ? Theme.elevated : over ? Theme.elevated : Theme.surfaceRaised
            }
        }
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
    }
}
