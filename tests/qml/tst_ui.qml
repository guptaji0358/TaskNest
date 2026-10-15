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

    // Shortcuts only fire in the active window; make sure the test window is it.
    // If another application owns the foreground (a person is using the
    // machine), the test is skipped visibly instead of failing spuriously.
    function activateWindow() {
        tc.Window.window.raise()
        tc.Window.window.requestActivate()
        for (let i = 0; i < 30 && !tc.Window.window.active; ++i)
            wait(100)
        if (!tc.Window.window.active)
            skip("the test window could not be activated (another application has the foreground)")
    }

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
        // A window that is not exposed is not rendered, so delegates would never
        // be created; make sure the test window is up front before each test.
        tc.Window.window.raise()
        tc.Window.window.requestActivate()
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
        compare(titles(), ["yesterday", "today open", "today done", "finished"])

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
        activateWindow()
        view.forceActiveFocus()
        keyClick(Qt.Key_F, Qt.ControlModifier)
        tryVerify(function () { return find("searchField").activeFocus }, 2000, "Ctrl+F focuses search")
        keyClick(Qt.Key_N, Qt.ControlModifier)
        tryVerify(function () { return find("addField").activeFocus }, 2000, "Ctrl+N focuses the add field")
    }

    function test_ctrlNLeavesCompletedViewForAddableOne() {
        activateWindow()
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

    // ---- workflow: order, keyboard, safety -------------------------------------------

    function test_listIsInWorkflowOrder() {
        seed("undated")
        seed("due later", "", dayAt(5, 10))
        seed("due soon", "", dayAt(1, 10))
        seed("finished early", "", dayAt(0, 8), true)
        compare(titles(), ["due soon", "due later", "undated", "finished early"])

        // completing a task from the UI moves it to the bottom
        mouseClick(find("taskCheck", row(0)))
        tryCompare(fixture.model, "count", 4)
        // open tasks first (by due date), finished ones below (also by due date)
        compare(titles(), ["due later", "undated", "finished early", "due soon"])
    }

    function test_downArrowEntersListAndMovesBetweenRows() {
        seed("one"); seed("two"); seed("three")
        find("searchField").forceActiveFocus()
        keyClick(Qt.Key_Down)
        tryVerify(function () { return row(0).activeFocus }, 2000, "first row focused")
        keyClick(Qt.Key_Down)
        tryVerify(function () { return row(1).activeFocus }, 2000, "second row focused")
        keyClick(Qt.Key_Up)
        tryVerify(function () { return row(0).activeFocus }, 2000)
        keyClick(Qt.Key_Up) // above the first row: back to search
        tryVerify(function () { return find("searchField").activeFocus }, 2000)
    }

    function test_spaceTogglesFocusedTask() {
        seed("toggle me")
        find("searchField").forceActiveFocus()
        keyClick(Qt.Key_Down)
        tryVerify(function () { return row(0).activeFocus }, 2000)
        keyClick(Qt.Key_Space)
        tryVerify(function () { return fixture.stored()[0].completed }, 2000, "completed by Space")
        keyClick(Qt.Key_Space)
        tryVerify(function () { return !fixture.stored()[0].completed }, 2000, "reopened by Space")
    }

    function test_viewShortcuts() {
        activateWindow()
        view.forceActiveFocus()
        keyClick(Qt.Key_2, Qt.ControlModifier)
        tryCompare(find("viewTitle"), "text", "Today")
        keyClick(Qt.Key_3, Qt.ControlModifier)
        tryCompare(find("viewTitle"), "text", "Upcoming")
        keyClick(Qt.Key_4, Qt.ControlModifier)
        tryCompare(find("viewTitle"), "text", "Completed")
        keyClick(Qt.Key_5, Qt.ControlModifier)
        tryCompare(find("viewTitle"), "text", "Inbox")
        keyClick(Qt.Key_1, Qt.ControlModifier)
        tryCompare(find("viewTitle"), "text", "All Tasks")
    }

    function test_shortcutReferenceOpensWithF1AndCloses() {
        activateWindow()
        view.forceActiveFocus()
        keyClick(Qt.Key_F1)
        const dialog = find("shortcutsDialog") || null
        tryVerify(function () { return find("shortcutsClose") !== null && find("shortcutsClose").visible }, 2000)
        mouseClick(find("shortcutsClose"))
        tryVerify(function () { return find("shortcutsClose") === null || !find("shortcutsClose").visible }, 2000)
    }

    function test_doubleClickOnDeleteConfirmDeletesOnceWithoutError() {
        seed("only once")
        mouseClick(find("deleteButton", row(0)))
        tryCompare(view.deleteConfirmDialog, "visible", true)
        const confirm = find("deleteConfirm")
        mouseClick(confirm)
        mouseClick(confirm) // second click lands while the dialog is closing
        tryCompare(view.deleteConfirmDialog, "visible", false)
        compare(storedTitles(), [])
        compare(find("errorBanner").visible, false)
    }

    function test_doubleClickOnSaveDoesNotError() {
        seed("save twice")
        const editor = openEditor()
        find("editorTitle").text = "saved once"
        const save = find("editorSave")
        mouseClick(save)
        mouseClick(save)
        tryCompare(editor, "visible", false)
        compare(storedTitles(), ["saved once"])
        compare(find("errorBanner").visible, false)
    }

    function test_enterInNotesAddsNewlineAndCtrlEnterSaves() {
        seed("notes task")
        const editor = openEditor()
        const notes = find("editorNotes")
        notes.forceActiveFocus()
        keyClick("a")
        keyClick(Qt.Key_Return)          // plain Enter: a new line, not a save
        keyClick("b")
        compare(editor.visible, true)
        compare(notes.text, "a\nb")

        keyClick(Qt.Key_Return, Qt.ControlModifier) // Ctrl+Enter saves
        tryCompare(editor, "visible", false)
        compare(fixture.stored()[0].notes, "a\nb")
    }

    function test_escapeClosesEditorWithoutSaving() {
        seed("keep this title")
        const editor = openEditor()
        find("editorTitle").text = "changed but cancelled"
        find("editorTitle").forceActiveFocus()
        keyClick(Qt.Key_Escape)
        tryCompare(editor, "visible", false)
        compare(storedTitles(), ["keep this title"])
    }

    function test_searchIgnoresCaseAndClearingRestoresEverything() {
        seed(String.fromCharCode(0x00DC) + "ber uns")
        seed("Plain task")
        seed("Another")
        find("searchField").text = String.fromCharCode(0x00FC) + "BER"
        compare(rowCount(), 1)
        find("searchField").text = "PLAIN"
        compare(rowCount(), 1)
        find("searchField").text = ""
        compare(rowCount(), 3)
    }

    function test_filteringNeverChangesStoredTasks() {
        seed("open one", "", dayAt(2, 9))
        seed("done one", "", null, true)
        const before = JSON.stringify(fixture.stored())
        const filter = find("filterBox")
        filter.currentIndex = 1; filter.activated(1)
        find("searchField").text = "open"
        clickNav("completed")
        clickNav("today")
        clickNav("upcoming")
        find("searchField").text = ""
        clickNav("all")
        compare(JSON.stringify(fixture.stored()), before)
    }

    // ---- quick add and undo delete ---------------------------------------------------

    function openDetails() {
        mouseClick(find("quickAddToggle"))
        tryVerify(function () { return find("quickAddPanel").visible }, 2000, "details panel opens")
    }
    function fill(name, text) {
        const field = find(name)
        field.forceActiveFocus()
        field.text = text
    }

    function test_quickAddWithTitleOnlyStillWorksWithPanelClosed() {
        compare(find("quickAddPanel").visible, false)
        addViaUi("Fast one")
        compare(storedTitles(), ["Fast one"])
        const t = fixture.stored()[0]
        compare(t.listName, "Inbox")
        compare(t.priority, 0)
        compare(t.hasDue, false)
        compare(t.hasReminder, false)
    }

    function test_quickAddWithDetailsPersistsEverything() {
        openDetails()
        find("addField").forceActiveFocus()
        find("addField").text = "Detailed task"
        find("quickPriority").currentIndex = 2
        fill("quickList", "Work")
        fill("quickDate", "2031-07-08")
        fill("quickTime", "16:45")
        fill("quickReminderDate", "2031-07-08")
        fill("quickReminderTime", "15:30")
        fill("quickNotes", "bring the slides")
        find("addField").forceActiveFocus()
        keyClick(Qt.Key_Return) // Enter in the title field submits everything

        compare(storedTitles(), ["Detailed task"])
        const t = fixture.stored()[0]
        compare(t.listName, "Work")
        compare(t.priority, 2)
        compare(t.notes, "bring the slides")
        verify(t.hasDue)
        compare(t.dueAt.getFullYear(), 2031); compare(t.dueAt.getMonth(), 6); compare(t.dueAt.getDate(), 8)
        compare(t.dueAt.getHours(), 16); compare(t.dueAt.getMinutes(), 45)
        verify(t.hasReminder)
        compare(t.reminderAt.getHours(), 15); compare(t.reminderAt.getMinutes(), 30)
        compare(t.reminderSent, false)

        // the form was cleared for the next task, and the title field too
        compare(find("addField").text, "")
        compare(find("quickList").text, "")
        compare(find("quickPriority").currentIndex, 0)
        // the new list appears in the sidebar
        tryVerify(function () { return find("nav_list:Work") !== null }, 2000)
    }

    function test_quickAddInvalidDetailsKeepEverythingAndStoreNothing() {
        openDetails()
        find("addField").text = "Needs a valid date"
        fill("quickDate", "31/12/2030")
        fill("quickNotes", "keep me")
        find("addField").forceActiveFocus()
        keyClick(Qt.Key_Return)
        compare(storedTitles(), [])
        compare(find("quickAddError").visible, true)
        verify(find("quickAddError").text.indexOf("Due date") >= 0)
        compare(find("addField").text, "Needs a valid date")
        compare(find("quickNotes").text, "keep me")

        // an impossible reminder time is refused too
        fill("quickDate", "")
        fill("quickReminderDate", "2030-01-01")
        fill("quickReminderTime", "99:99")
        find("addField").forceActiveFocus()
        keyClick(Qt.Key_Return)
        compare(storedTitles(), [])
        verify(find("quickAddError").text.indexOf("Reminder time") >= 0)

        // fixing it works
        fill("quickReminderTime", "09:15")
        find("addField").forceActiveFocus()
        keyClick(Qt.Key_Return)
        compare(storedTitles(), ["Needs a valid date"])
    }

    function test_quickAddRepeatedSubmitCreatesOneTask() {
        find("addField").forceActiveFocus()
        find("addField").text = "Only once"
        const button = find("addButton")
        mouseClick(button)
        mouseClick(button)
        mouseClick(button)
        keyClick(Qt.Key_Return)
        compare(storedTitles(), ["Only once"])
        compare(find("addMessage").visible, false) // a repeat is not reported as a mistake
    }

    function test_quickAddEmptyTitleStillWarnsWithDetailsOpen() {
        openDetails()
        fill("quickList", "Work")
        find("addField").forceActiveFocus()
        keyClick(Qt.Key_Return)
        compare(find("addMessage").visible, true)
        compare(storedTitles(), [])
    }

    function test_quickAddShortcutOpensDetailsAndFocusesTitle() {
        activateWindow()
        view.forceActiveFocus()
        keyClick(Qt.Key_N, Qt.ControlModifier | Qt.ShiftModifier)
        tryVerify(function () { return find("quickAddPanel").visible }, 2000)
        tryVerify(function () { return find("addField").activeFocus }, 2000)
    }

    function test_quickAddNotShownInViewTellsTheUser() {
        seed("existing")
        clickNav("today")
        // a task with a far-future due date does not belong in "Today"
        openDetails()
        find("addField").text = "Next month"
        fill("quickDate", "2031-01-01")
        find("addField").forceActiveFocus()
        keyClick(Qt.Key_Return)
        compare(storedTitles().length, 2)
        tryVerify(function () { return find("toast").visible }, 2000)
        verify(find("toastText").text.indexOf("not shown in this view") >= 0)
    }

    function deleteFirstRow() {
        mouseClick(find("deleteButton", row(0)))
        tryCompare(view.deleteConfirmDialog, "visible", true)
        mouseClick(find("deleteConfirm"))
        tryCompare(view.deleteConfirmDialog, "visible", false)
    }

    function test_undoRestoresTheDeletedTaskWithAllFields() {
        const due = new Date(2031, 4, 6, 14, 30)
        const reminder = new Date(2031, 4, 6, 14, 0)
        verify(fixture.model.addTask("Precious", "line one", "Work", 2, due, reminder))
        const before = fixture.stored()[0]

        deleteFirstRow()
        compare(storedTitles(), [])
        tryVerify(function () { return find("toast").visible }, 2000)
        verify(find("toastText").text.indexOf("Precious") >= 0)
        compare(find("toastAction").visible, true)

        mouseClick(find("toastAction"))
        tryCompare(fixture.model, "count", 1)
        const after = fixture.stored()[0]
        compare(after.id, before.id)
        compare(after.title, "Precious")
        compare(after.notes, "line one")
        compare(after.listName, "Work")
        compare(after.priority, 2)
        compare(after.dueAt.getTime(), before.dueAt.getTime())
        compare(after.reminderAt.getTime(), before.reminderAt.getTime())
        compare(after.reminderSent, false)
        compare(find("errorBanner").visible, false)
    }

    function test_undoCannotBeUsedTwice() {
        seed("Once")
        deleteFirstRow()
        tryVerify(function () { return find("toastAction").visible }, 2000)
        const action = find("toastAction")
        mouseClick(action)
        mouseClick(action) // second click: nothing left to undo, nothing duplicated
        tryCompare(fixture.model, "count", 1)
        compare(storedTitles(), ["Once"])
        compare(find("errorBanner").visible, false)
        compare(fixture.model.canUndo, false)
    }

    function test_undoShortcutWorksWhenNotTyping() {
        activateWindow()
        seed("Ctrl+Z me")
        deleteFirstRow()
        tryVerify(function () { return find("toast").visible }, 2000)
        find("taskList").forceActiveFocus()
        keyClick(Qt.Key_Z, Qt.ControlModifier)
        tryCompare(fixture.model, "count", 1)
        compare(storedTitles(), ["Ctrl+Z me"])
    }

    function test_undoOfferExpires() {
        seed("Too late")
        view.undoWindowMs = 400
        deleteFirstRow()
        tryVerify(function () { return find("toast").visible }, 2000)
        tryVerify(function () { return !find("toast").visible }, 3000, "the offer disappears")
        compare(fixture.model.canUndo, false)
        compare(storedTitles(), [])
        view.undoWindowMs = 8000
    }

    function test_newerDeleteReplacesTheUndoOffer() {
        seed("first"); seed("second")
        deleteFirstRow()
        deleteFirstRow()
        tryVerify(function () { return find("toast").visible }, 2000)
        verify(find("toastText").text.indexOf("second") >= 0)
        mouseClick(find("toastAction"))
        tryCompare(fixture.model, "count", 1)
        compare(storedTitles(), ["second"])
    }

    // ---- keyboard-only use ------------------------------------------------------------

    function test_keyboardOnlyDeleteWithEnterKeepsFocusOnTheList() {
        seed("one"); seed("two"); seed("three"); seed("four")
        view.focusRow(1)   // works even though the delegates are not created yet
        tryVerify(function () { return row(1).activeFocus }, 2000)
        keyClick(Qt.Key_Delete)
        tryCompare(view.deleteConfirmDialog, "opened", true) // transition done, focus on Cancel
        keyClick(Qt.Key_Tab)          // Cancel -> Delete
        keyClick(Qt.Key_Return)       // Enter activates the focused button
        tryCompare(fixture.model, "count", 3)
        compare(storedTitles(), ["one", "three", "four"])
        // focus is back on a task row, not lost
        tryVerify(function () { return row(1) !== null && row(1).activeFocus }, 2000, "focus restored to the neighbouring row")
        keyClick(Qt.Key_Down)
        tryVerify(function () { return row(2).activeFocus }, 2000)
    }

    function test_enterOnDefaultCancelKeepsTheTaskAndTheFocus() {
        seed("keep"); seed("also keep")
        view.focusRow(0)
        tryVerify(function () { return row(0).activeFocus }, 2000)
        keyClick(Qt.Key_Delete)
        tryCompare(view.deleteConfirmDialog, "opened", true) // transition done, focus on Cancel
        keyClick(Qt.Key_Return)       // the safe default (Cancel) is focused
        tryCompare(view.deleteConfirmDialog, "visible", false)
        compare(storedTitles().length, 2)
        tryVerify(function () { return row(0).activeFocus }, 2000)
    }

    function test_editorOpensOnTitleAndEscapeReturnsToTheRow() {
        seed("edit me")
        view.focusRow(0)
        tryVerify(function () { return row(0).activeFocus }, 2000)
        keyClick(Qt.Key_Return)
        tryCompare(view.editorDialog, "visible", true)
        tryVerify(function () { return find("editorTitle").activeFocus }, 2000)
        keyClick(Qt.Key_Escape)
        tryCompare(view.editorDialog, "visible", false)
        tryVerify(function () { return row(0).activeFocus }, 2000)
    }

    // ---- completed-task cleanup ---------------------------------------------

    function openCleanup() {
        mouseClick(find("settingsButton"))
        tryCompare(view.cleanupSettings.settingsDialog, "opened", true)
    }
    function closeCleanup() {
        mouseClick(find("cleanupClose"))
        tryCompare(view.cleanupSettings.settingsDialog, "visible", false)
    }

    function test_cleanupSettingsShowTheDefaultPolicy() {
        openCleanup()
        compare(find("cleanupCurrent").text, "Current setting: Never automatically delete")
        compare(find("cleanupModeBox").currentIndex, 0)
        compare(find("cleanupSave").enabled, false)      // nothing to save yet
        compare(find("cleanupDaysBox").visible, false)
        compare(find("clearCompletedButton").enabled, false) // no completed tasks
        closeCleanup()
    }

    function test_cleanupSettingsReachableByKeyboardAndEscapeCloses() {
        find("settingsButton").forceActiveFocus()
        verify(find("settingsButton").activeFocus)
        keyClick(Qt.Key_Space)
        tryCompare(view.cleanupSettings.settingsDialog, "opened", true)
        verify(find("cleanupModeBox").activeFocus) // first control is focused
        keyClick(Qt.Key_Escape)
        tryCompare(view.cleanupSettings.settingsDialog, "visible", false)
    }

    function test_clearCompletedAsksFirstAndKeepsActiveTasks() {
        seed("open task")
        seed("done 1", "", null, true)
        seed("done 2", "", null, true)
        openCleanup()
        compare(find("clearCompletedButton").enabled, true)

        mouseClick(find("clearCompletedButton"))
        tryCompare(view.cleanupSettings.clearConfirmDialog, "opened", true)
        verify(find("clearCancel").activeFocus) // the safe choice has focus
        mouseClick(find("clearCancel"))
        tryCompare(view.cleanupSettings.clearConfirmDialog, "visible", false)
        compare(storedTitles().length, 3)         // cancelled: nothing deleted

        mouseClick(find("clearCompletedButton"))
        tryCompare(view.cleanupSettings.clearConfirmDialog, "opened", true)
        mouseClick(find("clearConfirmButton"))
        tryCompare(view.cleanupSettings.clearConfirmDialog, "visible", false)
        compare(storedTitles(), ["open task"])
        compare(fixture.model.canUndo, false)     // permanent
        compare(find("clearCompletedButton").enabled, false)
        closeCleanup()
        compare(rowCount(), 1)
    }

    function test_savingAPolicyThatDeletesExplainsAndNeedsConfirmation() {
        seed("open task")
        seed("done already", "", null, true)
        openCleanup()

        find("cleanupModeBox").currentIndex = 1
        verify(find("cleanupSave").enabled)
        verify(find("cleanupImpact").visible)    // the consequence is shown up front
        mouseClick(find("cleanupSave"))
        tryCompare(view.cleanupSettings.saveConfirmDialog, "opened", true)
        compare(fixture.model.cleanupMode, 0)    // nothing saved or deleted yet
        compare(storedTitles().length, 2)

        mouseClick(find("cleanupSaveCancel"))
        tryCompare(view.cleanupSettings.saveConfirmDialog, "visible", false)
        compare(fixture.model.cleanupMode, 0)
        compare(storedTitles().length, 2)

        mouseClick(find("cleanupSave"))
        tryCompare(view.cleanupSettings.saveConfirmDialog, "opened", true)
        mouseClick(find("cleanupSaveConfirmButton"))
        tryCompare(view.cleanupSettings.settingsDialog, "visible", false)
        compare(fixture.model.cleanupMode, 1)
        compare(storedTitles(), ["open task"])
    }

    function test_immediatePolicyRemovesACompletedTaskFromTheList() {
        seed("finish me")
        seed("stay open")
        verify(fixture.model.setCleanupPolicy(1, 0))
        compare(rowCount(), 2)
        // "finish me" has the earlier id; both are undated, so it is first.
        mouseClick(find("taskCheck", row(0)))
        tryCompare(fixture.model, "count", 1)
        compare(storedTitles(), ["stay open"])
        compare(fixture.model.canUndo, false)
        verify(!find("emptyState").visible)
        keyClick(Qt.Key_Z, Qt.ControlModifier)   // Ctrl+Z has nothing to restore
        compare(storedTitles(), ["stay open"])
    }

    function test_saveRetentionPolicyWithNoImpactSavesDirectly() {
        seed("open task")
        openCleanup()
        find("cleanupModeBox").currentIndex = 2
        verify(find("cleanupDaysBox").visible)
        find("cleanupDaysBox").currentIndex = 2   // 14 days
        compare(find("cleanupImpact").visible, false)
        mouseClick(find("cleanupSave"))
        tryCompare(view.cleanupSettings.settingsDialog, "visible", false)
        compare(fixture.model.cleanupMode, 2)
        compare(fixture.model.cleanupDays, 14)
        compare(storedTitles(), ["open task"])
        openCleanup()
        compare(find("cleanupCurrent").text, "Current setting: Delete 14 days after completion")
        closeCleanup()
    }

    // ---- date and time pickers -----------------------------------------------

    // The editor scrolls; bring a field into the visible part before clicking it.
    function reveal(item) {
        const flick = find("editorScroll").contentItem
        const y = item.mapToItem(flick.contentItem, 0, 0).y
        flick.contentY = Math.max(0, Math.min(y - 40, flick.contentHeight - flick.height))
        wait(30)
    }

    function openCalendarFor(objectName) {
        const field = find(objectName)
        reveal(field)
        mouseClick(find("dateOpenButton", field))
        tryCompare(field.calendarPopup, "opened", true)
        return field
    }

    function test_calendarPicksADayAndNavigatesMonths() {
        seed("cal")
        const editor = openEditor()
        const field = find("editorDate")
        field.text = "2031-01-31"

        openCalendarFor("editorDate")
        compare(find("calendarTitle").text, "January 2031")
        mouseClick(find("calendarNext"))
        compare(find("calendarTitle").text, "February 2031")
        mouseClick(find("calendarPrev"))
        mouseClick(find("calendarPrev"))
        compare(find("calendarTitle").text, "December 2030")
        mouseClick(find("calendarNext"))
        mouseClick(find("calendarNext"))
        compare(find("calendarTitle").text, "February 2031")
        mouseClick(find("calendarDay_2031-2-28")) // last day of a non-leap February
        tryCompare(field.calendarPopup, "visible", false)
        compare(field.text, "2031-02-28")

        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)
        const due = fixture.stored()[0].dueAt
        compare(due.getFullYear(), 2031)
        compare(due.getMonth(), 1)
        compare(due.getDate(), 28)
        compare(due.getHours(), 23) // date-only keeps its end-of-day default
        compare(due.getMinutes(), 59)
    }

    function test_calendarClearAndCancelAreAvailable() {
        seed("cal")
        openEditor()
        const field = find("editorReminderDate")
        field.text = "2031-06-15"

        openCalendarFor("editorReminderDate")
        mouseClick(find("calendarCancel"))
        tryCompare(field.calendarPopup, "visible", false)
        compare(field.text, "2031-06-15") // unchanged

        openCalendarFor("editorReminderDate")
        mouseClick(find("calendarClear"))
        tryCompare(field.calendarPopup, "visible", false)
        compare(field.text, "")
        mouseClick(find("editorCancel"))
    }

    function test_calendarWorksFromTheKeyboard() {
        seed("cal")
        openEditor()
        const field = find("editorDate")
        field.text = "2031-01-31"
        field.forceActiveFocus()

        keyClick(Qt.Key_Down, Qt.AltModifier)
        tryCompare(field.calendarPopup, "opened", true)
        keyClick(Qt.Key_PageDown)         // month end is kept inside the month
        keyClick(Qt.Key_Left)
        keyClick(Qt.Key_Return)
        tryCompare(field.calendarPopup, "visible", false)
        compare(field.text, "2031-02-27")
        verify(field.activeFocus)         // focus returns to the field

        keyClick(Qt.Key_F4)
        tryCompare(field.calendarPopup, "opened", true)
        keyClick(Qt.Key_Escape)
        tryCompare(field.calendarPopup, "visible", false)
        compare(field.text, "2031-02-27")
        mouseClick(find("editorCancel"))
    }

    function test_clickingTheFieldOpensTheCalendarAndKeepsTyping() {
        seed("cal")
        openEditor()
        const field = find("editorDate")
        mouseClick(field)
        tryCompare(field.calendarPopup, "opened", true)
        verify(field.activeFocus) // typing still goes to the field
        keyClick(Qt.Key_Escape)
        mouseClick(find("editorCancel"))
    }

    function test_timePickerSetsAndLeavesBlankUntilSet() {
        seed("time")
        const editor = openEditor()
        const field = find("editorReminderTime")
        compare(field.text, "")
        reveal(field)

        mouseClick(find("timeOpenButton", field))
        tryCompare(field.timePopup, "opened", true)
        compare(find("timeHour").value, 9)   // starts at the field's default
        compare(find("timeMinute").value, 0)
        mouseClick(find("timeCancel"))
        tryCompare(field.timePopup, "visible", false)
        compare(field.text, "")             // nothing was set by looking

        mouseClick(find("timeOpenButton", field))
        tryCompare(field.timePopup, "opened", true)
        find("timeHour").value = 7
        find("timeMinute").value = 5
        mouseClick(find("timeSet"))
        tryCompare(field.timePopup, "visible", false)
        compare(field.text, "07:05")

        find("editorReminderDate").text = "2031-05-06"
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)
        const r = fixture.stored()[0].reminderAt
        compare(r.getHours(), 7)
        compare(r.getMinutes(), 5)
    }

    function test_manualTimeVariantsAreNormalizedOnLeavingTheField() {
        seed("manual")
        openEditor()
        const time = find("editorTime")
        time.forceActiveFocus()
        time.text = "9-05"
        find("editorTitle").forceActiveFocus()
        compare(time.text, "09:05")

        time.forceActiveFocus()
        time.text = "14-30-00"
        find("editorTitle").forceActiveFocus()
        compare(time.text, "14:30")

        time.forceActiveFocus()
        time.text = "14:30:45"
        find("editorTitle").forceActiveFocus()
        compare(time.text, "14:30:45")

        const date = find("editorDate")
        date.forceActiveFocus()
        date.text = "2031/3/7"
        find("editorTitle").forceActiveFocus()
        compare(date.text, "2031-03-07")
        mouseClick(find("editorCancel"))
    }

    function test_invalidManualInputKeepsTheTypedText() {
        seed("bad")
        const editor = openEditor()
        const date = find("editorDate")
        const time = find("editorTime")
        date.forceActiveFocus()
        date.text = "2031-02-30"
        time.forceActiveFocus()
        time.text = "24:00"
        find("editorTitle").forceActiveFocus()
        compare(date.text, "2031-02-30")   // not rolled into March, not cleared
        compare(time.text, "24:00")
        compare(date.invalid, true)
        compare(time.invalid, true)

        mouseClick(find("editorSave"))
        compare(editor.visible, true)
        verify(find("editorError").text.indexOf("does not exist") >= 0)
        compare(date.text, "2031-02-30")
        compare(fixture.stored()[0].hasDue, false)
        mouseClick(find("editorCancel"))
    }

    function test_hyphenTimeWithSecondsIsStored() {
        seed("secs")
        const editor = openEditor()
        find("editorDate").text = "2031-09-10"
        find("editorTime").text = "08-15-30"
        mouseClick(find("editorSave"))
        tryCompare(editor, "visible", false)
        const due = fixture.stored()[0].dueAt
        compare(due.getHours(), 8)
        compare(due.getMinutes(), 15)
        compare(due.getSeconds(), 30)

        // and it is shown with its seconds when edited again
        openEditor()
        compare(find("editorTime").text, "08:15:30")
        mouseClick(find("editorCancel"))
    }

    // ---- floating widget -----------------------------------------------------

    Component { id: spyComponent; SignalSpy {} }
    Component { id: listModelComponent; ListModel {} }
    readonly property var widgetComponent:
        Qt.createComponent(Qt.resolvedUrl("../../scripts/main/FloatingWidget.qml"))

    // Shows the widget (the controller's visibility drives its window).
    function makeWidget(extra) {
        fixture.widget.widgetVisible = true
        const props = Object.assign({ controller: fixture.widget, taskModel: fixture.model }, extra || {})
        const w = createTemporaryObject(widgetComponent, null, props)
        verify(w !== null, widgetComponent.errorString())
        tryCompare(w, "visible", true)
        waitForRendering(w.contentItem)
        return w
    }
    function wfind(w, name) { return find(name, w.contentItem) }
    function spyOn(target, signalName) {
        return createTemporaryObject(spyComponent, tc, { target: target, signalName: signalName })
    }

    function test_widgetShowsTodayAndUpcomingAndCanBeHidden() {
        seed("due today", "", dayAt(0, 18))
        seed("due later", "", dayAt(3, 9))
        seed("undated")
        const w = makeWidget()
        tryCompare(wfind(w, "widgetTodayList"), "count", 1)
        compare(wfind(w, "widgetUpcomingList").count, 1)

        mouseClick(wfind(w, "widgetHide"))
        tryCompare(w, "visible", false)
        compare(fixture.widget.widgetVisible, false)
        compare(storedTitles().length, 3) // hiding never touches tasks
        compare(rowCount(), 3)
    }

    function test_widgetCompletesAndAddsTasksThroughTheMainModel() {
        seed("finish me", "", dayAt(0, 23))
        const w = makeWidget()
        const list = wfind(w, "widgetTodayList")
        tryCompare(list, "count", 1)
        tryVerify(function () { return list.itemAtIndex(0) !== null })

        mouseClick(find("widgetTaskCheck", list.itemAtIndex(0)))
        tryCompare(list, "count", 0)
        compare(fixture.stored()[0].completed, true)

        // quick add from the widget: due today by default, so it shows up there
        w.quickField.text = "Added in widget"
        w.quickField.accepted()
        tryCompare(list, "count", 1)
        compare(storedTitles(), ["finish me", "Added in widget"])
        compare(fixture.stored()[1].hasDue, true)
        compare(w.quickField.text, "")
        compare(rowCount(), 2) // the main window shows both
    }

    function test_widgetQuickAddWithoutDateAndEmptyTitle() {
        const w = makeWidget()
        w.quickField.accepted() // empty: nothing happens
        compare(storedTitles(), [])
        find("widgetDueChoice", w.contentItem).currentIndex = 2
        w.quickField.text = "No date task"
        w.quickField.accepted()
        compare(storedTitles(), ["No date task"])
        compare(fixture.stored()[0].hasDue, false)
        compare(wfind(w, "widgetTodayList").count, 0)
    }

    function test_widgetSectionsFollowThePreferences() {
        const w = makeWidget()
        const c = fixture.widget
        compare(wfind(w, "widgetTodaySection").visible, true)
        c.showToday = false
        compare(wfind(w, "widgetTodaySection").visible, false)
        c.showUpcoming = false
        c.showQuickAdd = false
        compare(w.quickField.visible, false)
        compare(wfind(w, "widgetNothingShown").visible, true)
        c.showToday = true
        compare(wfind(w, "widgetNothingShown").visible, false)
        compare(wfind(w, "widgetTodaySection").visible, true)
    }

    function test_widgetSettingsPopupChangesAndPersistsPreferences() {
        const w = makeWidget()
        mouseClick(wfind(w, "widgetSettingsButton"))
        tryVerify(function () { return wfind(w, "widgetOption_showUpcoming") !== null
                                       && wfind(w, "widgetOption_showUpcoming").visible })
        mouseClick(wfind(w, "widgetOption_showUpcoming"))
        compare(fixture.widget.showUpcoming, false)
        compare(wfind(w, "widgetUpcomingSection").visible, false)

        fixture.widget.opacity = 0.7
        compare(w.opacity, 0.7)
        fixture.widget.opacity = 0.01 // clamped to a readable minimum
        verify(w.opacity >= 0.6)

        fixture.widget.compact = true
        compare(w.compact, true)
        const before = String(w.surfaceColor)
        fixture.widget.surface = 1
        verify(String(w.surfaceColor) !== before)
    }

    function test_widgetOpensATaskInTheMainWindow() {
        seed("open from widget", "", dayAt(0, 20))
        const w = makeWidget()
        const list = wfind(w, "widgetTodayList")
        tryCompare(list, "count", 1)
        tryVerify(function () { return list.itemAtIndex(0) !== null })
        const spy = spyOn(w, "openTaskRequested")
        mouseClick(find("widgetTaskOpen", list.itemAtIndex(0)))
        tryCompare(spy, "count", 1)

        view.openTask(spy.signalArguments[0][0])
        tryCompare(view.editorDialog, "visible", true)
        compare(find("editorTitle").text, "open from widget")
        mouseClick(find("editorCancel"))
    }

    function test_widgetListsFiredRemindersAndRelaysTheButtons() {
        const reminders = createTemporaryObject(listModelComponent, tc)
        const w = makeWidget({ remindersModel: reminders })
        compare(wfind(w, "widgetReminders").visible, false) // only when relevant

        reminders.append({ taskId: 7, title: "Call back", when: new Date() })
        tryCompare(wfind(w, "widgetReminders"), "visible", true)
        const done = spyOn(w, "reminderDoneRequested")
        const dismiss = spyOn(w, "reminderDismissRequested")
        waitForRendering(w.contentItem)
        wait(100) // let the new row get its geometry
        mouseClick(wfind(w, "widgetReminderDone"))
        compare(done.count, 1)
        compare(done.signalArguments[0][0], 7)
        mouseClick(wfind(w, "widgetReminderDismiss"))
        compare(dismiss.count, 1)

        fixture.widget.showReminders = false
        compare(wfind(w, "widgetReminders").visible, false)
    }

    function test_widgetOffScreenPositionIsRecovered() {
        fixture.widget.x = 90000
        fixture.widget.y = -70000
        const w = makeWidget()
        const s = w.screen
        verify(w.x >= s.virtualX - 400 && w.x < s.virtualX + s.width, "x on screen: " + w.x)
        verify(w.y >= s.virtualY && w.y < s.virtualY + s.height, "y on screen: " + w.y)
    }

    function test_widgetCanBeClosedWhileTheMainWindowKeepsWorking() {
        seed("survivor", "", dayAt(0, 12))
        const w = makeWidget()
        w.close() // the system close path: hides, does not quit or delete
        tryCompare(w, "visible", false)
        compare(fixture.widget.widgetVisible, false)
        w.destroy()
        wait(50)
        // the main view and model are unaffected and still update the widget views
        addViaUi("after the widget is gone")
        compare(storedTitles().length, 2)
        verify(fixture.model.setCompleted(fixture.stored()[0].id, true))
        compare(rowCount(), 2)
    }
}
