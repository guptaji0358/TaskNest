import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "DateTimeInput.js" as DT

// Month calendar. Keyboard: arrows move by day/week, PageUp/PageDown by month,
// Home/End to the start/end of the month, Enter or Space picks the highlighted
// day, T = today, Delete = clear (when clearable), Escape = cancel.
FocusScope {
    id: root

    // The currently chosen day (year/month/day), or all 0 for none.
    property int selectedYear: 0
    property int selectedMonth: 0
    property int selectedDay: 0
    property bool clearable: true

    // The month on show and the highlighted ("cursor") day.
    property int viewYear: new Date().getFullYear()
    property int viewMonth: new Date().getMonth() + 1
    property int cursorYear: viewYear
    property int cursorMonth: viewMonth
    property int cursorDay: 1

    signal picked(int year, int month, int day)
    signal cleared()
    signal cancelled()

    implicitWidth: 292
    implicitHeight: column.implicitHeight

    readonly property var cells: DT.monthCells(viewYear, viewMonth)
    readonly property var weekdays: ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"]

    // Shows the month of the given day and puts the cursor on it.
    function showDay(year, month, day) {
        cursorYear = year
        cursorMonth = month
        cursorDay = day
        viewYear = year
        viewMonth = month
    }

    function showToday() {
        const t = new Date()
        showDay(t.getFullYear(), t.getMonth() + 1, t.getDate())
    }

    // Opens on the selected day, or today when there is none.
    function reset() {
        if (selectedYear > 0)
            showDay(selectedYear, selectedMonth, selectedDay)
        else
            showToday()
    }

    function shiftMonth(delta) {
        let m = viewMonth - 1 + delta
        let y = viewYear + Math.floor(m / 12)
        m = ((m % 12) + 12) % 12
        if (y < DT.MIN_YEAR || y > DT.MAX_YEAR)
            return
        viewYear = y
        viewMonth = m + 1
    }

    // Moves the cursor by whole days (or months) and follows it with the view.
    function moveCursor(days, months) {
        let d = new Date(cursorYear, cursorMonth - 1 + (months || 0), cursorDay, 12, 0, 0, 0)
        if (months) {
            // stay inside the target month: Jan 31 + 1 month = Feb 28/29
            const target = ((cursorMonth - 1 + months) % 12 + 12) % 12
            if (d.getMonth() !== target)
                d = new Date(d.getFullYear(), d.getMonth(), 0, 12, 0, 0, 0)
        }
        if (days)
            d = new Date(d.getFullYear(), d.getMonth(), d.getDate() + days, 12, 0, 0, 0)
        if (d.getFullYear() < DT.MIN_YEAR || d.getFullYear() > DT.MAX_YEAR)
            return
        showDay(d.getFullYear(), d.getMonth() + 1, d.getDate())
    }

    function pickCursor() {
        picked(cursorYear, cursorMonth, cursorDay)
    }

    Keys.onPressed: (event) => {
        let handled = true
        switch (event.key) {
        case Qt.Key_Left: moveCursor(-1, 0); break
        case Qt.Key_Right: moveCursor(1, 0); break
        case Qt.Key_Up: moveCursor(-7, 0); break
        case Qt.Key_Down: moveCursor(7, 0); break
        case Qt.Key_PageUp: moveCursor(0, -1); break
        case Qt.Key_PageDown: moveCursor(0, 1); break
        case Qt.Key_Home: showDay(viewYear, viewMonth, 1); break
        case Qt.Key_End: showDay(viewYear, viewMonth, DT.daysInMonth(viewYear, viewMonth)); break
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space: pickCursor(); break
        case Qt.Key_T: showToday(); break
        case Qt.Key_Delete:
            if (clearable)
                cleared()
            break
        case Qt.Key_Escape: cancelled(); break
        default: handled = false
        }
        event.accepted = handled
    }

    ColumnLayout {
        id: column
        width: parent.width
        spacing: Theme.s2

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s1

            TNIconButton {
                objectName: "calendarPrev"
                iconName: "chevron"
                rotation: 90
                tip: "Previous month"
                focusPolicy: Qt.NoFocus
                onClicked: root.shiftMonth(-1)
            }
            TNLabel {
                objectName: "calendarTitle"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                role: "title"
                text: DT.monthName(root.viewMonth) + " " + root.viewYear
                Accessible.role: Accessible.Heading
            }
            TNIconButton {
                objectName: "calendarNext"
                iconName: "chevron"
                rotation: -90
                tip: "Next month"
                focusPolicy: Qt.NoFocus
                onClicked: root.shiftMonth(1)
            }
        }

        Row {
            Layout.fillWidth: true
            Repeater {
                model: root.weekdays
                delegate: TNLabel {
                    required property string modelData
                    width: column.width / 7
                    horizontalAlignment: Text.AlignHCenter
                    role: "overline"
                    text: modelData
                }
            }
        }

        // Cells are placed explicitly (7 columns x 6 rows).
        Item {
            id: grid
            objectName: "calendarGrid"
            Layout.fillWidth: true
            Layout.preferredHeight: 6 * 34
            Repeater {
                model: root.cells
                delegate: Item {
                    id: cell
                    required property var modelData
                    required property int index
                    width: grid.width / 7
                    height: 34
                    x: (index % 7) * width
                    y: Math.floor(index / 7) * height

                    readonly property bool isCursor: modelData.year === root.cursorYear
                        && modelData.month === root.cursorMonth && modelData.day === root.cursorDay
                    readonly property bool isSelected: modelData.year === root.selectedYear
                        && modelData.month === root.selectedMonth && modelData.day === root.selectedDay
                    readonly property bool isToday: {
                        const t = new Date()
                        return modelData.year === t.getFullYear() && modelData.month === t.getMonth() + 1
                               && modelData.day === t.getDate()
                    }

                    Accessible.role: Accessible.Button
                    Accessible.name: DT.monthName(modelData.month) + " " + modelData.day + ", " + modelData.year
                    Accessible.selected: isSelected

                    Rectangle {
                        anchors.centerIn: parent
                        width: 30
                        height: 30
                        radius: 15
                        color: cell.isSelected ? Theme.accent
                               : area.containsMouse ? Theme.surfaceHover : "transparent"
                        border.width: cell.isCursor && root.activeFocus ? 2
                                      : cell.isToday ? 1 : 0
                        border.color: cell.isCursor && root.activeFocus ? Theme.focus : Theme.accentLine
                    }
                    TNLabel {
                        anchors.centerIn: parent
                        text: cell.modelData.day
                        role: "label"
                        color: cell.isSelected ? Theme.accentText
                               : cell.modelData.inMonth ? Theme.text : Theme.textFaint
                        font.weight: cell.isSelected || cell.isToday ? Font.DemiBold : Font.Normal
                    }
                    MouseArea {
                        id: area
                        objectName: "calendarDay_" + cell.modelData.year + "-" + cell.modelData.month
                                    + "-" + cell.modelData.day
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.picked(cell.modelData.year, cell.modelData.month, cell.modelData.day)
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2

            TNButton {
                objectName: "calendarToday"
                variant: "ghost"
                text: "Today"
                implicitHeight: 32
                focusPolicy: Qt.NoFocus
                onClicked: {
                    const t = new Date()
                    root.picked(t.getFullYear(), t.getMonth() + 1, t.getDate())
                }
            }
            Item { Layout.fillWidth: true }
            TNButton {
                objectName: "calendarClear"
                visible: root.clearable
                variant: "ghost"
                text: "Clear"
                implicitHeight: 32
                focusPolicy: Qt.NoFocus
                onClicked: root.cleared()
            }
            TNButton {
                objectName: "calendarCancel"
                variant: "ghost"
                text: "Cancel"
                implicitHeight: 32
                focusPolicy: Qt.NoFocus
                onClicked: root.cancelled()
            }
        }
    }
}
