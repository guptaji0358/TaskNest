import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Compact "add a task" window opened by the global shortcut or a
// tasknest://quickadd link. It is its own small top-level window, so the main
// window and the floating widget are neither shown nor raised by it. It uses
// the same QuickAddPanel and TaskListModel::addTask path as the main view.
Window {
    id: win

    required property var taskModel
    property var hotkey: null // GlobalHotkey (gives the keyboard back on close)

    property string errorText: ""
    property bool closing: false
    property double lastAddAt: 0

    transientParent: null
    title: "TaskNest Quick Add"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"
    visible: false
    width: 520
    height: frame.implicitHeight + 24
    minimumWidth: 380

    // Shows the window (once), centers it in the upper part of the screen and
    // puts the keyboard in the title field. A repeat call just refocuses.
    function present() {
        if (!visible) {
            titleField.clear()
            panel.reset()
            errorText = ""
            detailsOpen = false
            closing = false
            const screen = Qt.application.screens.length > 0 ? Qt.application.screens[0] : null
            if (screen) {
                x = screen.virtualX + (screen.width - width) / 2
                y = screen.virtualY + screen.height * 0.22
            }
            show()
        }
        raise()
        requestActivate()
        titleField.forceActiveFocus()
    }

    function cancel() {
        if (closing)
            return
        closing = true
        hide()
        if (hotkey)
            hotkey.restoreForeground()
    }

    property bool detailsOpen: false

    function submit() {
        if (closing)
            return
        // A repeat Enter right after a successful add must not add again.
        if (Date.now() - lastAddAt < 700)
            return
        errorText = ""
        const title = titleField.text.trim()
        if (title === "") {
            errorText = "Enter a task title first."
            titleField.forceActiveFocus()
            return
        }
        const details = panel.collect()
        if (details.error) {
            detailsOpen = true
            errorText = details.error
            return
        }
        if (taskModel.addTask(title, details.notes, details.list.trim(), details.priority,
                              details.due, details.reminder)) {
            lastAddAt = Date.now()
            closing = true
            hide()
            if (hotkey)
                hotkey.restoreForeground()
        } else {
            errorText = taskModel.lastError // stays open with the text kept
        }
    }

    // Closing it any other way (Alt+F4, or the application quitting) is a
    // cancel. The close is accepted: vetoing it would keep the application
    // alive after the main window is closed.
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

        // lit top edge
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

        // Controls inherit this palette (popups, check boxes).
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
                TNLabel {
                    Layout.fillWidth: true
                    role: "overline"
                    text: "Quick add"
                }
                TNIconButton {
                    objectName: "captureDetails"
                    iconName: "chevron"
                    rotation: win.detailsOpen ? 180 : 0
                    tip: win.detailsOpen ? "Hide details" : "Add details"
                    onClicked: win.detailsOpen = !win.detailsOpen
                }
            }

            TNTextField {
                id: titleField
                objectName: "captureTitle"
                Layout.fillWidth: true
                look: "title"
                placeholderText: "What needs doing?"
                onAccepted: win.submit()
                Keys.onEscapePressed: win.cancel()
            }

            QuickAddPanel {
                id: panel
                objectName: "capturePanel"
                Layout.fillWidth: true
                expanded: win.detailsOpen
                errorText: win.errorText.indexOf("Due") === 0 || win.errorText.indexOf("Reminder") === 0
                           ? win.errorText : ""
                onSubmitted: win.submit()
            }

            TNLabel {
                objectName: "captureError"
                Layout.fillWidth: true
                visible: win.errorText !== "" && panel.errorText === ""
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
                    text: "Enter to add · Esc to cancel"
                }
                TNButton {
                    objectName: "captureCancel"
                    variant: "ghost"
                    text: "Cancel"
                    onClicked: win.cancel()
                }
                TNButton {
                    objectName: "captureAdd"
                    variant: "primary"
                    iconName: "check"
                    text: "Add task"
                    onClicked: win.submit()
                }
            }
        }
    }
}
