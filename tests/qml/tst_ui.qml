import QtQuick
import QtQuick.Controls
import QtTest

// Drives the real MainView.qml with simulated mouse/keyboard input.
TestCase {
    id: tc
    name: "TaskNestUi"
    width: 800   // the application's minimum window size
    height: 520
    visible: true
    when: windowShown

    property var view: null
    readonly property var viewComponent:
        Qt.createComponent(Qt.resolvedUrl("../../scripts/main/MainView.qml"))

    // ---- helpers ----------------------------------------------------------

    // Searches the window content, then open popups (the overlay).
    function find(name, root) {
        if (!root) {
            const inContent = find(name, tc.Window.window.contentItem)
            return inContent ? inContent : find(name, Overlay.overlay)
        }
        const start = root
        if (start.objectName === name)
            return start
        const kids = start.children
        for (let i = 0; i < kids.length; ++i) {
            const found = find(name, kids[i])
            if (found)
                return found
        }
        if (start.contentItem && start.contentItem !== start)
            return find(name, start.contentItem)
        return null
    }

    // Delegates are created on the next frame, so wait for them.
    function row(i) {
        tryVerify(function () { return find("taskList").itemAtIndex(i) !== null })
        return find("taskList").itemAtIndex(i)
    }
    function rowCount() { return find("taskList").count }
    function titles() {
        const result = []
        for (let i = 0; i < fixture.model.count; ++i)
            result.push(fixture.model.data(fixture.model.index(i, 0), Qt.DisplayRole))
        return result
    }
    function storedTitles() { return fixture.stored().map(t => t.title) }

    function addViaUi(text) {
        const field = find("addField")
        field.forceActiveFocus()
        field.text = text
        keyClick(Qt.Key_Return)
    }
    function clickNav(key) {
        tryVerify(function () { return find("nav_" + key) !== null }, 2000, "nav item " + key)
        const item = find("nav_" + key)
        mouseClick(item)
    }
    function seed(title, list, dueDate, completed) {
        verify(fixture.model.addTask(title, "", list || "", 0, dueDate || new Date(NaN)))
        if (completed) {
            const all = fixture.stored()
            verify(fixture.model.setCompleted(all[all.length - 1].id, true))
        }
    }
    function dayAt(offsetDays, hour) {
        const d = new Date()
        d.setDate(d.getDate() + offsetDays)
        d.setHours(hour, 0, 0, 0)
        return d
    }

    function init() {
        verify(fixture.reset())
        view = createTemporaryObject(viewComponent, tc, {
            width: tc.width, height: tc.height,
            databaseError: "", databasePath: "C:/test/tasknest.db",
            taskModel: fixture.model,
            reminderScheduler: fixture.scheduler
        })
        verify(view !== null, viewComponent.errorString())
        waitForRendering(view)
    }

    // ---- task creation ------------------------------------------------------

    function test_addWithEnterClearsFieldAndPersists() {
        addViaUi("  Buy milk  ")
        compare(rowCount(), 1)
        compare(find("addField").text, "")
        compare(storedTitles(), ["Buy milk"])
        compare(find("emptyState").visible, false)
    }

    function test_addWithButton() {
        find("addField").text = "Via button"
        mouseClick(find("addButton"))
        compare(storedTitles(), ["Via button"])
    }

    function test_blankTitleIsRejectedWithMessage() {
        addViaUi("   ")
        compare(storedTitles(), [])
        compare(find("addMessage").visible, true)
        verify(find("addMessage").text.length > 0)
        // typing clears the message again
        find("addField").forceActiveFocus()
        keyClick("a")
        compare(find("addMessage").visible, false)
    }

    function test_sqlLikeTitleIsPlainText() {
        addViaUi("x'); DROP TABLE tasks;--")
        compare(storedTitles(), ["x'); DROP TABLE tasks;--"])
        compare(find("taskTitle", row(0)).text, "x'); DROP TABLE tasks;--")
    }

    // ---- completion and deletion ---------------------------------------------

    function test_completionPersistsAndIsStyled() {
        seed("Task A")
        const title = find("taskTitle", row(0))
        compare(title.font.strikeout, false)
        mouseClick(find("taskCheck", row(0)))
        tryCompare(fixture.stored()[0], "completed", true)
        compare(title.font.strikeout, true)
        verify(title.opacity < 1)
        // and back
        mouseClick(find("taskCheck", row(0)))
        compare(fixture.stored()[0].completed, false)
        compare(title.font.strikeout, false)
    }

    function test_deleteNeedsConfirmation() {
        seed("Keep me")
        mouseClick(find("deleteButton", row(0)))
        const dialog = view.deleteConfirmDialog
        tryCompare(dialog, "visible", true)
        compare(storedTitles(), ["Keep me"]) // nothing deleted yet

        mouseClick(find("deleteCancel"))
        tryCompare(dialog, "visible", false)
        compare(storedTitles(), ["Keep me"])
        compare(rowCount(), 1)

        mouseClick(find("deleteButton", row(0)))
        tryCompare(dialog, "visible", true)
        mouseClick(find("deleteConfirm"))
        tryCompare(dialog, "visible", false)
        compare(storedTitles(), [])
        compare(rowCount(), 0)
    }

    function test_clickingCheckboxNeverOpensDeleteDialog() {
        seed("Only toggle")
        mouseClick(find("taskCheck", row(0)))
        compare(view.deleteConfirmDialog.visible, false)
        compare(view.editorDialog.visible, false)
    }

    // ---- editing --------------------------------------------------------------

    function openEditor() {
        mouseClick(find("editButton", row(0)))
        const editor = view.editorDialog
        tryCompare(editor, "visible", true)
        return editor
    }

    function test_editSavesAllFields() {
        seed("Original")
        const editor = openEditor()
        compare(find("editorTitle").text, "Original")

        find("editorTitle").text = "Changed title"
        find("editorNotes").text = "some notes"
        find("editorPriority").currentIndex = 2
        find("editorList").text = "Work"
        find("editorDate").text = "2031-02-03"
        find("editorTime").text = "14:30"
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)

        const t = fixture.stored()[0]
        compare(t.title, "Changed title")
        compare(t.notes, "some notes")
        compare(t.priority, 2)
        compare(t.listName, "Work")
        verify(t.hasDue)
        compare(t.dueAt.getFullYear(), 2031)
        compare(t.dueAt.getMonth(), 1)
        compare(t.dueAt.getDate(), 3)
        compare(t.dueAt.getHours(), 14)
        compare(t.dueAt.getMinutes(), 30)
    }

    function test_cancelDoesNotModifyTask() {
        seed("Untouched")
        const editor = openEditor()
        find("editorTitle").text = "Should not be saved"
        find("editorDate").text = "2031-01-01"
        mouseClick(find("editorCancel"))
        tryCompare(editor, "visible", false)
        compare(storedTitles(), ["Untouched"])
        compare(fixture.stored()[0].hasDue, false)
    }

    function test_editorValidationKeepsDialogOpen() {
        seed("Valid")
        const editor = openEditor()
        const err = find("editorError")

        find("editorTitle").text = "   "
        mouseClick(find("editorSave"))
        compare(err.visible, true)
        compare(editor.visible, true)

        find("editorTitle").text = "Valid"
        find("editorDate").text = "31/12/2030"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("Due date") >= 0)

        find("editorDate").text = "2030-02-31"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("does not exist") >= 0)

        find("editorDate").text = "2030-02-10"
        find("editorTime").text = "25:99"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("time must") >= 0)

        find("editorDate").text = ""
        find("editorTime").text = "10:00"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("due date") >= 0)

        compare(editor.visible, true)
        compare(fixture.stored()[0].hasDue, false) // nothing was saved
        mouseClick(find("editorCancel"))
    }

    function test_editorCanClearDueDate() {
        seed("Dated", "", dayAt(3, 10))
        compare(fixture.stored()[0].hasDue, true)
        const editor = openEditor()
        compare(find("editorDate").text !== "", true) // prefilled
        find("editorDate").text = ""
        find("editorTime").text = ""
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)
        compare(fixture.stored()[0].hasDue, false)
    }

    function test_doubleClickOpensEditor() {
        seed("Double")
        mouseDoubleClickSequence(row(0), 200, row(0).height / 2)
        tryCompare(view.editorDialog, "visible", true)
        mouseClick(find("editorCancel"))
    }

    // ---- views, filters, search ----------------------------------------------------

    function seedViews() {
        seed("today open", "", dayAt(0, 23))
        seed("today done", "", dayAt(0, 23), true)
        seed("tomorrow", "Work", dayAt(1, 12))
        seed("yesterday", "", dayAt(-1, 12))
        seed("no date", "Home")
        seed("finished", "", null, true)
    }

    function test_sidebarViews() {
        seedViews()
        compare(rowCount(), 6) // All Tasks

        clickNav("today")
        compare(titles(), ["today open", "today done"])
        compare(find("viewTitle").text, "Today")

        clickNav("upcoming")
        compare(titles(), ["tomorrow"])

        clickNav("completed")
        compare(titles(), ["today done", "finished"])

        clickNav("list:Inbox")
        compare(titles(), ["today open", "today done", "yesterday", "finished"])

        clickNav("list:Work")
        compare(titles(), ["tomorrow"])
        clickNav("list:Home")
        compare(titles(), ["no date"])

        clickNav("all")
        compare(rowCount(), 6)
    }

    function test_filterComposesWithSearch() {
        seedViews()
        const filter = find("filterBox")
        const search = find("searchField")

        filter.currentIndex = 1
        filter.activated(1)
        compare(fixture.model.count, 4) // active tasks

        search.text = "today"
        compare(titles(), ["today open"])

        filter.currentIndex = 2
        filter.activated(2)
        compare(titles(), ["today done"])

        clickNav("today") // Today shows all states again, search still applies
        compare(fixture.model.completionFilter, 0)
        compare(titles(), ["today open", "today done"])

        search.text = ""
        compare(fixture.model.count, 2)
    }

    function test_filterIsLockedForUpcomingAndCompleted() {
        const filter = find("filterBox")
        compare(filter.enabled, true)
        clickNav("upcoming")
        compare(filter.enabled, false)
        clickNav("completed")
        compare(filter.enabled, false)
        compare(filter.currentIndex, 2)
        clickNav("all")
        compare(filter.enabled, true)
    }

    function test_searchTreatsInputAsLiteral() {
        seed("100% done")
        seed("100x done")
        find("searchField").text = "100%"
        compare(titles(), ["100% done"])
        find("searchField").text = "' OR 1=1 --"
        compare(rowCount(), 0)
    }

    function test_addInTodayViewGetsDueToday() {
        clickNav("today")
        addViaUi("Due today by default")
        compare(rowCount(), 1)
        const t = fixture.stored()[0]
        verify(t.hasDue)
        compare(t.dueAt.getDate(), new Date().getDate())
    }

    function test_addInListViewUsesThatList() {
        seed("seed", "Work")
        clickNav("list:Work")
        addViaUi("Another work task")
        compare(fixture.stored()[1].listName, "Work")
        compare(rowCount(), 2)
    }

    function test_addDisabledInCompletedView() {
        clickNav("completed")
        compare(find("addField").enabled, false)
        compare(find("addButton").enabled, false)
    }

    function test_emptyListFallsBackToAllTasks() {
        seed("only", "Temp")
        clickNav("list:Temp")
        compare(find("viewTitle").text, "Temp")
        verify(fixture.model.removeTask(fixture.stored()[0].id))
        tryCompare(find("viewTitle"), "text", "All Tasks")
    }

    // ---- empty states -----------------------------------------------------------------

    function test_emptyStates() {
        const empty = find("emptyState")
        compare(empty.visible, true)
        verify(empty.text.indexOf("no tasks yet") >= 0)

        seed("exists")
        compare(empty.visible, false)

        find("searchField").text = "zzz"
        compare(empty.visible, true)
        verify(empty.text.indexOf("zzz") >= 0)
        find("searchField").text = ""

        clickNav("today")
        verify(empty.text.indexOf("due today") >= 0)
        clickNav("upcoming")
        verify(empty.text.indexOf("upcoming") >= 0)
        clickNav("completed")
        verify(empty.text.indexOf("completed") >= 0)
    }

    // ---- layout at the minimum window size ------------------------------------------------

    function test_layoutFitsMinimumSize() {
        seed("A task with a fairly long title that must be elided rather than overflow the row", "", dayAt(2, 9))
        compare(view.width, 800)
        const names = ["addField", "addButton", "searchField", "filterBox", "taskList", "viewTitle", "taskCount"]
        for (let i = 0; i < names.length; ++i) {
            const item = find(names[i])
            const p = item.mapToItem(view, 0, 0)
            verify(p.x >= 0 && p.x + item.width <= view.width + 0.5, names[i] + " horizontally inside")
            verify(p.y >= 0 && p.y + item.height <= view.height + 0.5, names[i] + " vertically inside")
        }
        const r = row(0)
        const del = find("deleteButton", r)
        const p = del.mapToItem(r, 0, 0)
        verify(p.x + del.width <= r.width + 0.5, "row buttons fit inside the row")
        verify(find("taskTitle", r).width > 100, "title keeps usable width")
    }

    // ---- reminders ------------------------------------------------------------------

    function test_editorSetsShowsAndClearsReminder() {
        seed("Remind me")
        let editor = openEditor()
        compare(find("editorReminderDate").text, "")
        find("editorReminderDate").text = "2031-05-06"
        find("editorReminderTime").text = "07:45"
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)

        let t = fixture.stored()[0]
        verify(t.hasReminder)
        compare(t.reminderAt.getFullYear(), 2031)
        compare(t.reminderAt.getMonth(), 4)
        compare(t.reminderAt.getDate(), 6)
        compare(t.reminderAt.getHours(), 7)
        compare(t.reminderAt.getMinutes(), 45)
        compare(t.reminderSent, false)
        verify(find("reminderLabel", row(0)).visible)

        // reopening shows the stored (local) time; saving untouched keeps it
        editor = openEditor()
        compare(find("editorReminderDate").text, "2031-05-06")
        compare(find("editorReminderTime").text, "07:45")
        find("editorTitle").text = "Renamed"
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)
        t = fixture.stored()[0]
        compare(t.title, "Renamed")
        compare(t.reminderAt.getHours(), 7)

        // clear
        editor = openEditor()
        find("editorReminderDate").text = ""
        find("editorReminderTime").text = ""
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)
        compare(fixture.stored()[0].hasReminder, false)
        compare(find("reminderLabel", row(0)).visible, false)
    }

    function test_reminderDateOnlyDefaultsToNineAndValidates() {
        seed("Date only")
        const editor = openEditor()
        const err = find("editorError")

        find("editorReminderDate").text = "2031-01-01"
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)
        compare(fixture.stored()[0].reminderAt.getHours(), 9)
        compare(fixture.stored()[0].reminderAt.getMinutes(), 0)

        openEditor()
        find("editorReminderDate").text = "yesterday"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("Reminder date") >= 0)
        find("editorReminderDate").text = "2031-02-30"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("does not exist") >= 0)
        find("editorReminderDate").text = "2031-02-03"
        find("editorReminderTime").text = "7pm"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("Reminder time") >= 0)
        find("editorReminderDate").text = ""
        find("editorReminderTime").text = "08:00"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("reminder date") >= 0)
        compare(view.editorDialog.visible, true)
        mouseClick(find("editorCancel"))
        // nothing from the failed attempts was stored
        compare(fixture.stored()[0].reminderAt.getDate(), 1)
    }

    function test_cancelKeepsReminder() {
        seed("Keep reminder")
        verify(fixture.model.updateTaskWithReminder(fixture.stored()[0].id, "Keep reminder", "", "Inbox",
                                                    0, new Date(NaN), new Date(2032, 0, 2, 3, 4)))
        const editor = openEditor()
        find("editorReminderDate").text = ""
        find("editorReminderTime").text = ""
        mouseClick(find("editorCancel"))
        tryCompare(editor, "visible", false)
        compare(fixture.stored()[0].hasReminder, true)
        compare(fixture.stored()[0].reminderAt.getMinutes(), 4)
    }

    function test_firedReminderShowsBannerAndDismissStoresIt() {
        seed("Pay rent")
        const id = fixture.stored()[0].id
        const past = new Date(Date.now() - 60 * 1000)
        verify(fixture.model.updateTaskWithReminder(id, "Pay rent", "", "Inbox", 0, new Date(NaN), past))

        // The scheduler (driven by the model's change signal) announces it.
        tryVerify(function () { return find("reminderBanner") !== null }, 3000)
        verify(find("reminderText").text.indexOf("Pay rent") >= 0)
        compare(fixture.stored()[0].reminderSent, false) // not "sent" until handled

        mouseClick(find("reminderDismiss"))
        tryVerify(function () { return find("reminderBanner") === null })
        compare(fixture.stored()[0].reminderSent, true)
        compare(fixture.stored()[0].completed, false)
    }

    function test_reminderCompleteButtonCompletesTask() {
        seed("Call back")
        const id = fixture.stored()[0].id
        verify(fixture.model.updateTaskWithReminder(id, "Call back", "", "Inbox", 0, new Date(NaN),
                                                    new Date(Date.now() - 1000)))
        tryVerify(function () { return find("reminderBanner") !== null }, 3000)
        waitForRendering(view) // let the banner's layout settle before clicking in it
        mouseClick(find("reminderComplete"))
        tryVerify(function () { return find("reminderBanner") === null })
        compare(fixture.stored()[0].completed, true)
        compare(fixture.stored()[0].reminderSent, true)
    }

    function test_completingTaskRemovesItsBanner() {
        seed("Done elsewhere")
        const id = fixture.stored()[0].id
        verify(fixture.model.updateTaskWithReminder(id, "Done elsewhere", "", "Inbox", 0, new Date(NaN),
                                                    new Date(Date.now() - 1000)))
        tryVerify(function () { return find("reminderBanner") !== null }, 3000)
        waitForRendering(view)
        mouseClick(find("taskCheck", row(0)))
        tryVerify(function () { return find("reminderBanner") === null })
    }

    // ---- resizing ---------------------------------------------------------------------

    function test_editorFitsInsideMinimumWindow() {
        seed("Editor size", "", dayAt(1, 9))
        const editor = openEditor()
        // Force the error line to show too: the tallest the dialog gets.
        find("editorDate").text = "bad"
        mouseClick(find("editorSave"))
        const overlay = Overlay.overlay
        verify(editor.height <= overlay.height, "editor height " + editor.height + " <= " + overlay.height)
        verify(editor.y >= 0, "editor top inside window")
        // Save/Cancel buttons are reachable (inside the window)
        const save = find("editorSave")
        const p = save.mapToItem(overlay, 0, save.height)
        verify(p.y <= overlay.height, "Save button visible: bottom " + p.y)
        mouseClick(find("editorCancel"))
    }

    function test_layoutAdaptsToLargeWindow() {
        seed("A", "", dayAt(1, 9))
        seed("B")
        view.width = 1400
        view.height = 900
        waitForRendering(view)
        const list = find("taskList")
        const field = find("addField")
        const r = row(0)
        const column = find("contentColumn")
        const cp = column.mapToItem(view, 0, 0)
        const rp = r.mapToItem(view, 0, 0)
        verify(list.width > 900, "list grows with the window")
        // rows line up with the column above them (same right edge)
        verify(Math.abs((rp.x + r.width) - (cp.x + column.width)) <= 2,
               "row right edge " + (rp.x + r.width) + " vs column " + (cp.x + column.width))
        const del = find("deleteButton", r)
        const dp = del.mapToItem(r, 0, 0)
        verify(dp.x + del.width <= r.width + 0.5)
    }

    // ---- polish ---------------------------------------------------------------------

    function test_taskTurnsOverdueWhileWindowIsOpen() {
        const soon = new Date(Date.now() + 1200)
        verify(fixture.model.addTask("About to be late", "", "", 0, soon))
        const text = find("dueText", row(0))
        verify(text.text.indexOf("Overdue") < 0, "not overdue yet: " + text.text)
        tryVerify(function () { return text.text.indexOf("Overdue") === 0 }, 4000, "turns overdue by itself")
    }

    function test_validationHintDoesNotShiftLayoutAndClearsOnViewChange() {
        seed("A task")
        const yBefore = find("taskList").mapToItem(view, 0, 0).y
        addViaUi("   ")
        compare(find("addMessage").visible, true)
        compare(find("taskList").mapToItem(view, 0, 0).y, yBefore) // nothing moved
        clickNav("completed")
        compare(find("addMessage").visible, false) // stale hint is gone
    }

    function test_rowsAlignWithControlsEvenWhenScrolling() {
        for (let i = 0; i < 30; ++i)
            seed("Task " + i)
        const column = find("contentColumn")
        const cp = column.mapToItem(view, 0, 0)
        const r = row(0)
        const rp = r.mapToItem(view, 0, 0)
        verify(find("taskList").contentHeight > find("taskList").height, "list scrolls")
        verify(Math.abs((rp.x + r.width) - (cp.x + column.width)) <= 2)
    }

    function test_noMatchesUsesNeutralToken() {
        seed("Something")
        find("searchField").text = "qqq"
        const empty = find("emptyState")
        compare(empty.visible, true)
        compare(empty.accented, false)
        find("searchField").text = ""
        fixture.reset()
    }

    function test_longTitleOpensAtItsStart() {
        seed("An extremely long task title that certainly does not fit on one line of the editor field at all")
        openEditor()
        compare(find("editorTitle").cursorPosition, 0)
        mouseClick(find("editorCancel"))
    }

    // ---- reliability ----------------------------------------------------------------

    function test_keyboardShortcutsFocusSearchAndAdd() {
        view.forceActiveFocus()
        keyClick(Qt.Key_F, Qt.ControlModifier)
        tryVerify(function () { return find("searchField").activeFocus }, 2000, "Ctrl+F focuses search")
        keyClick(Qt.Key_N, Qt.ControlModifier)
        tryVerify(function () { return find("addField").activeFocus }, 2000, "Ctrl+N focuses the add field")
    }

    function test_ctrlNLeavesCompletedViewForAddableOne() {
        clickNav("completed")
        compare(find("addField").enabled, false)
        keyClick(Qt.Key_N, Qt.ControlModifier)
        tryCompare(find("viewTitle"), "text", "All Tasks")
        tryVerify(function () { return find("addField").activeFocus }, 2000)
    }

    function test_extremeDatesAreHandled() {
        seed("Dates")
        let editor = openEditor()
        const err = find("editorError")

        // year 0001 is not representable the way the form parses it: refused, not mangled
        find("editorDate").text = "0001-01-01"
        mouseClick(find("editorSave"))
        verify(err.text.indexOf("does not exist") >= 0, err.text)
        compare(editor.visible, true)

        // the far future is accepted and stored
        find("editorDate").text = "9999-12-31"
        find("editorTime").text = "10:00"
        find("editorReminderDate").text = "9999-12-30"
        find("editorReminderTime").text = "08:00"
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)
        const t = fixture.stored()[0]
        compare(t.dueAt.getFullYear(), 9999)
        compare(t.reminderAt.getDate(), 30)
        compare(find("errorBanner").visible, false)
    }
}
