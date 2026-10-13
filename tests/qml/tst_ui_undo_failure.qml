import QtQuick
import QtQuick.Controls
import QtTest

// A failed "Undo delete" must say so and keep the offer. Own file => own
// fixture, because it deliberately breaks the database connection.
TestCase {
    id: tc
    name: "TaskNestUiUndoFailure"
    width: 800
    height: 520
    visible: true
    when: windowShown

    readonly property var viewComponent:
        Qt.createComponent(Qt.resolvedUrl("../../scripts/main/MainView.qml"))

    function find(name, root) {
        if (!root) {
            const inContent = find(name, tc.Window.window.contentItem)
            return inContent ? inContent : find(name, Overlay.overlay)
        }
        if (root.objectName === name)
            return root
        for (let i = 0; i < root.children.length; ++i) {
            const found = find(name, root.children[i])
            if (found)
                return found
        }
        if (root.contentItem && root.contentItem !== root)
            return find(name, root.contentItem)
        return null
    }

    function test_failedUndoShowsAnErrorAndKeepsTheOffer() {
        verify(fixture.model.addTask("Recoverable"))
        const view = createTemporaryObject(viewComponent, tc, {
            width: tc.width, height: tc.height,
            databaseError: "", databasePath: "C:/test/tasknest.db",
            taskModel: fixture.model, reminderScheduler: fixture.scheduler
        })
        verify(view !== null, viewComponent.errorString())
        waitForRendering(view)
        tryVerify(function () { return find("taskList").itemAtIndex(0) !== null })

        mouseClick(find("deleteButton", find("taskList").itemAtIndex(0)))
        tryCompare(view.deleteConfirmDialog, "visible", true)
        mouseClick(find("deleteConfirm"))
        tryCompare(view.deleteConfirmDialog, "visible", false)
        tryVerify(function () { return find("toastAction") !== null && find("toastAction").visible }, 2000)

        fixture.breakDatabase() // restoring is impossible now
        mouseClick(find("toastAction"))
        tryVerify(function () { return find("errorBanner").visible }, 2000)
        verify(find("errorBannerText").text.length > 0)
        compare(fixture.model.canUndo, true)     // can be retried later
        compare(find("toastAction").visible, true)
    }
}
