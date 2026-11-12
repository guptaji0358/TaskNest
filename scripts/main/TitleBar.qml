import QtQuick
import QtQuick.Controls
import QtQuick.Window

// The window's own title bar, drawn in place of the system one. Dragging it
// moves the window (Aero Snap works because the move is the system's),
// double-click maximizes, and the three caption buttons follow the Windows 11
// layout. The maximize button is hit-tested natively by WindowChrome so the
// snap-layout flyout appears; `maximizeHovered` / `maximizePressed` carry its
// state back because the pointer never reaches QML there.
Rectangle {
    id: root

    property var window: null           // the top-level window to move
    property bool maximized: false
    property bool maximizeHovered: false
    property bool maximizePressed: false
    property string context: ""         // current view, shown beside the name
    property bool railMode: false

    property bool widgetAvailable: false
    property bool widgetActive: false

    signal toggleSidebarRequested()
    signal widgetToggleRequested()
    signal settingsRequested()
    signal minimizeRequested()
    signal maximizeRequested()
    signal closeRequested()

    // Where the maximize button is, in window coordinates (this bar sits at 0,0).
    readonly property rect maximizeRect: Qt.rect(width - 2 * Theme.captionButtonWidth, 0,
                                                 Theme.captionButtonWidth, height)

    implicitHeight: Theme.titleBarHeight
    color: Theme.bg

    // Drag and double-click, under everything else.
    Item {
        anchors.fill: parent
        DragHandler {
            target: null
            grabPermissions: PointerHandler.CanTakeOverFromAnything
            onActiveChanged: { if (active && root.window) root.window.startSystemMove() }
        }
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onDoubleTapped: root.maximizeRequested()
        }
    }

    Row {
        id: leftRow
        anchors.left: parent.left
        anchors.leftMargin: Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s2

        TNIconButton {
            objectName: "sidebarToggle"
            anchors.verticalCenter: parent.verticalCenter
            iconName: "panel"
            tip: root.railMode ? "Expand sidebar" : "Collapse sidebar"
            onClicked: root.toggleSidebarRequested()
        }
        Image {
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            source: "qrc:/icons/tasknest-256.png"
            sourceSize: Qt.size(40, 40)
            smooth: true
            mipmap: true
        }
        TNLabel {
            anchors.verticalCenter: parent.verticalCenter
            text: "TaskNest"
            role: "label"
            color: Theme.text
            font.weight: Font.DemiBold
        }
        TNLabel {
            visible: root.context !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: "›  " + root.context
            role: "label"
            color: Theme.textFaint
        }
    }

    // The otherwise empty middle of the bar; MainView parents its search field here.
    Item {
        id: searchSlot
        objectName: "titleSearchSlot"
        readonly property real leftEdge: leftRow.x + leftRow.width + Theme.s4
        readonly property real rightEdge: root.width - 3 * Theme.captionButtonWidth - 2 * Theme.s2 - actionsRow.width
        x: Math.max(leftEdge, (root.width - width) / 2)
        width: Math.max(0, Math.min(440, rightEdge - leftEdge))
        height: Theme.titleBarHeight - 10
        anchors.verticalCenter: parent.verticalCenter
        visible: width >= 160
    }
    property alias searchSlot: searchSlot

    // Window-level actions, left of the caption buttons.
    Row {
        id: actionsRow
        anchors.right: parent.right
        anchors.rightMargin: 3 * Theme.captionButtonWidth + Theme.s2
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.s1

        TNIconButton {
            objectName: "widgetToggle"
            visible: root.widgetAvailable
            iconName: "widget"
            tip: root.widgetActive ? "Hide floating widget (Ctrl+Shift+W)"
                                   : "Show floating widget (Ctrl+Shift+W)"
            active: root.widgetActive
            onClicked: root.widgetToggleRequested()
        }
        TNIconButton {
            objectName: "settingsButton"
            iconName: "settings"
            tip: "Settings"
            onClicked: root.settingsRequested()
        }
    }

    // Caption buttons
    component CaptionButton: Rectangle {
        id: btn
        property string iconName: ""
        property bool nativeHot: false     // hover reported by WindowChrome
        readonly property bool hot: hover.hovered || nativeHot
        property bool down: false
        property bool isClose: false
        property string tip: ""
        signal clicked()

        width: Theme.captionButtonWidth
        height: root.height
        color: isClose ? (down ? Qt.darker(Theme.captionCloseHover, 1.15) : hot ? Theme.captionCloseHover : "transparent")
                       : (down ? Theme.layerHigh : hot ? Theme.layerMid : "transparent")
        Accessible.role: Accessible.Button
        Accessible.name: tip

        Icon {
            anchors.centerIn: parent
            name: btn.iconName
            size: 14
            strokeWidth: 1.4
            color: btn.isClose && btn.hot ? "#FFFFFF" : Theme.text
        }
        HoverHandler { id: hover }
        TapHandler { onTapped: btn.clicked() }
    }

    Row {
        anchors.right: parent.right
        anchors.top: parent.top

        CaptionButton {
            objectName: "minimizeButton"
            iconName: "minimize"
            tip: "Minimize"
            onClicked: root.minimizeRequested()
        }
        CaptionButton {
            objectName: "maximizeButton"
            iconName: root.maximized ? "restore" : "maximize"
            tip: root.maximized ? "Restore" : "Maximize"
            nativeHot: root.maximizeHovered
            down: root.maximizePressed
            onClicked: root.maximizeRequested()
        }
        CaptionButton {
            objectName: "closeButton"
            iconName: "close"
            isClose: true
            tip: "Close"
            onClicked: root.closeRequested()
        }
    }
}
