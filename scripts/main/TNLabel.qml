import QtQuick
import QtQuick.Controls

// Text with the TaskNest type scale. `role` picks size, weight, family, color:
// display | heading | title | body | label | caption | overline
Label {
    id: root

    property string role: "body"

    readonly property var spec: {
        switch (role) {
        case "display":
            return { px: Theme.fontDisplay, w: Font.DemiBold, disp: true, c: Theme.text, ls: -0.3 }
        case "heading":
            return { px: Theme.fontHeading, w: Font.DemiBold, disp: true, c: Theme.text, ls: 0 }
        case "title":
            return { px: Theme.fontTitle, w: Font.Medium, disp: false, c: Theme.text, ls: 0 }
        case "label":
            return { px: Theme.fontLabel, w: Font.Medium, disp: false, c: Theme.textMuted, ls: 0 }
        case "caption":
            return { px: Theme.fontCaption, w: Font.Normal, disp: false, c: Theme.textMuted, ls: 0 }
        case "overline":
            return { px: Theme.fontOverline, w: Font.DemiBold, disp: false, c: Theme.textFaint, ls: 1.0 }
        default:
            return { px: Theme.fontBody, w: Font.Normal, disp: false, c: Theme.text, ls: 0 }
        }
    }

    color: spec.c
    font.family: spec.disp ? Theme.displayFamily : Theme.fontFamily
    font.pixelSize: spec.px
    font.weight: spec.w
    font.letterSpacing: spec.ls
    font.capitalization: role === "overline" ? Font.AllUppercase : Font.MixedCase
}
