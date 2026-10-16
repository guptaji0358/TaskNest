import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Review window for text captured from another application. Nothing is stored
// until the user presses Capture; Cancel / Esc discard everything. It is a
// separate small window: the main window and the widget are not shown.
Window {
    id: win

    required property var taskModel
    property var hotkey: null // GlobalHotkey of the capture shortcut (gives the keyboard back)

    property string source: ""
    property string notice: ""
    property string errorText: ""
    property bool closing: false
    property double lastAddAt: 0

    readonly property int maxChars: 10000

    transientParent: null
    title: "TaskNest Capture"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"
    visible: false
    width: 560
    height: frame.implicitHeight + 24
    minimumWidth: 400

    // result: { status, text, source, message, truncated } from SelectionCapture
    function present(result) {
        if (!visible) {
            closing = false
            errorText = ""
            const screen = Qt.application.screens.length > 0 ? Qt.application.screens[0] : null
            if (screen) {
                x = screen.virtualX + (screen.width - width) / 2
                y = screen.virtualY + screen.height * 0.18
            }
            show()
        }
        const ok = result && result.status === "ok"
        textArea.text = ok ? result.text : ""
        source = result && result.source ? result.source : ""
        titleField.text = ok ? deriveTitle(result.text) : ""
        notice = ok ? (result.truncated ? result.message : "")
                    : (result && result.message ? result.message : "")
        if (!ok && !(result && result.status === "password")) {
            // The application did not share its selection: say what to do instead.
            notice = (notice !== "" ? notice + " " : "")
                     + "Copy the text in that application (Ctrl+C), then press Paste below."
        }
        raise()
        requestActivate()
        if (ok)
            titleField.forceActiveFocus()
        else
            textArea.forceActiveFocus()
    }

    function deriveTitle(text) {
        const lines = String(text).split("\n")
        for (let i = 0; i < lines.length; ++i) {
            const line = lines[i].replace(/\s+/g, " ").trim()
            if (line !== "")
                return line.length > 80 ? line.slice(0, 79).trim() + "…" : line
        }
        return ""
    }

    function cancel() {
        if (closing)
            return
        closing = true
        hide()
        if (hotkey)
            hotkey.restoreForeground()
    }

    function submit() {
        if (closing || Date.now() - lastAddAt < 700)
            return
        errorText = ""
        const text = textArea.text.replace(/\r\n?/g, "\n").trim()
        let title = titleField.text.replace(/\s+/g, " ").trim()
        if (text === "") {
            errorText = "There is no text to capture."
            return
        }
        if (text.length > maxChars) {
            errorText = "The text is " + text.length + " characters; the limit is " + maxChars + "."
            return
        }
        if (title === "")
            title = deriveTitle(text)
        if (title.length > 500) {
            errorText = "The title is longer than 500 characters."
            return
        }
        const notes = source !== "" ? text + "\n\nSource: " + source : text
        if (taskModel.addTask(title, notes)) {
            lastAddAt = Date.now()
            closing = true
            hide()
            if (hotkey)
                hotkey.restoreForeground()
        } else {
            errorText = taskModel.lastError // stays open; nothing was saved
        }
    }

    // Closing it any other way (Alt+F4, the application quitting) is a cancel
    // and is accepted, so it never keeps the application alive.
    onClosing: (close) => {
        close.accepted = true
        cancel()
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: win.visible
        onActivated: win.cancel()
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        anchors.margins: 12
        radius: Theme.radiusLg
        color: Theme.elevated
        border.width: 1
        border.color: Theme.borderStrong
        implicitHeight: content.implicitHeight + Theme.s4 * 2

        Rectangle {
            height: 1
            anchors.top: parent.top
            anchors.topMargin: 1
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.radiusLg
            anchors.rightMargin: Theme.radiusLg
            color: Theme.highlightStrong
        }

        palette.window: Theme.bg
        palette.windowText: Theme.text
        palette.base: Theme.surfaceRaised
        palette.text: Theme.text
        palette.button: Theme.surfaceRaised
        palette.buttonText: Theme.text
        palette.highlight: Theme.accent
        palette.highlightedText: Theme.accentText
        palette.mid: Theme.borderStrong
        palette.dark: Theme.border

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: Theme.s4
            spacing: Theme.s2

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2
                Icon { name: "plus"; size: 16; color: Theme.accent }
                TNLabel { Layout.fillWidth: true; role: "overline"; text: "Capture selected text" }
            }

            TNLabel {
                objectName: "captureNotice"
                Layout.fillWidth: true
                visible: win.notice !== ""
                role: "caption"
                color: Theme.warning
                wrapMode: Text.Wrap
                text: win.notice
            }

            TNLabel { role: "overline"; text: "Task title" }
            TNTextField {
                id: titleField
                objectName: "selectionTitle"
                Layout.fillWidth: true
                maximumLength: 500
                placeholderText: "Task title"
                onAccepted: win.submit()
            }

            RowLayout {
                Layout.fillWidth: true
                TNLabel { Layout.fillWidth: true; role: "overline"; text: "Captured text (editable)" }
                TNButton {
                    objectName: "selectionPaste"
                    variant: "ghost"
                    text: "Paste"
                    implicitHeight: 28
                    ToolTip.visible: hovered
                    ToolTip.text: "Insert the text currently on the clipboard"
                    onClicked: {
                        textArea.forceActiveFocus()
                        textArea.paste()
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 130
                radius: Theme.radiusMd
                color: Theme.surfaceRaised
                border.width: textArea.activeFocus ? 2 : 1
                border.color: textArea.activeFocus ? Theme.focus : Theme.border
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 2
                    clip: true
                    TextArea {
                        id: textArea
                        objectName: "selectionText"
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        color: Theme.text
                        placeholderText: "Nothing captured yet"
                        placeholderTextColor: Theme.textFaint
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.accentText
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontBody
                        background: null
                        leftPadding: Theme.s3
                        rightPadding: Theme.s3
                        topPadding: Theme.s2
                        bottomPadding: Theme.s2
                        // Tab moves on; it must not type a tab into the text.
                        KeyNavigation.priority: KeyNavigation.BeforeItem
                        KeyNavigation.tab: addButton
                        KeyNavigation.backtab: titleField
                    }
                }
            }
            TNLabel {
                Layout.fillWidth: true
                role: "caption"
                color: textArea.text.length > win.maxChars ? Theme.danger : Theme.textFaint
                text: textArea.text.length + " of " + win.maxChars + " characters"
            }

            RowLayout {
                Layout.fillWidth: true
                visible: win.source !== ""
                spacing: Theme.s2
                Icon { name: "list"; size: 14; color: Theme.textFaint }
                TNLabel {
                    objectName: "selectionSource"
                    Layout.fillWidth: true
                    role: "caption"
                    elide: Text.ElideMiddle
                    color: Theme.textMuted
                    text: "Source: " + win.source
                }
            }

            TNLabel {
                objectName: "selectionError"
                Layout.fillWidth: true
                visible: win.errorText !== ""
                role: "caption"
                color: Theme.danger
                wrapMode: Text.Wrap
                text: win.errorText
                Accessible.role: Accessible.AlertMessage
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2
                TNLabel {
                    Layout.fillWidth: true
                    role: "caption"
                    color: Theme.textFaint
                    text: "Nothing is saved until you press Capture."
                }
                TNButton {
                    objectName: "selectionCancel"
                    variant: "ghost"
                    text: "Cancel"
                    onClicked: win.cancel()
                }
                TNButton {
                    id: addButton
                    objectName: "selectionCapture"
                    variant: "primary"
                    iconName: "check"
                    text: "Capture"
                    onClicked: win.submit()
                }
            }
        }
    }
}
