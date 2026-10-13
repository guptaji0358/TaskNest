import QtQuick
import QtQuick.Controls
import QtTest

// Failure behaviour: operations that fail must be visible and must not
// look like they succeeded. Runs in its own engine/database.
TestCase {
    id: tc
    name: "TaskNestUiFailure"
    width: 800
    height: 520
    visible: true
    when: windowShown

    readonly property var viewComponent:
        Qt.createComponent(Qt.resolvedUrl("../../scripts/main/MainView.qml"))

    function find(name, root) {
        const start = root ? root : tc.Window.window.contentItem
        if (start.objectName === name)
            return start
        for (let i = 0; i < start.children.length; ++i) {
            const found = find(name, start.children[i])
            if (found)
                return found
        }
        if (start.contentItem && start.contentItem !== start)
            return find(name, start.contentItem)
        return null
    }

    function test_failedAddShowsErrorAndKeepsText() {
        verify(fixture.model.addTask("existing"))
        const view = createTemporaryObject(viewComponent, tc, {
            width: tc.width, height: tc.height,
            databaseError: "", databasePath: "C:/test/tasknest.db",
            taskModel: fixture.model
        })
        verify(view !== null, viewComponent.errorString())
        waitForRendering(view)
        tryVerify(function () { return find("taskList").itemAtIndex(0) !== null })
        compare(find("errorBanner").visible, false)

        fixture.breakDatabase() // from here on every database call fails

        const field = find("addField")
        field.forceActiveFocus()
        field.text = "will fail"
        keyClick(Qt.Key_Return)

        compare(find("errorBanner").visible, true)
        verify(find("errorBannerText").text.length > 0)
        compare(field.text, "will fail")        // input is kept for a retry
        compare(find("taskList").count, 1)      // no phantom row was added

        // Completing also fails visibly and the checkbox returns to the stored state.
        mouseClick(find("dismissBanner"))
        compare(find("errorBanner").visible, false)
        const check = find("taskCheck", find("taskList").itemAtIndex(0))
        mouseClick(check)
        compare(find("errorBanner").visible, true)
        compare(check.checked, false)
    }

    function test_unavailableDatabaseShowsErrorOnly() {
        const view = createTemporaryObject(viewComponent, tc, {
            width: tc.width, height: tc.height,
            databaseError: "Database path is a directory: C:/x", databasePath: "",
            taskModel: null
        })
        verify(view !== null, viewComponent.errorString())
        const message = find("unavailableMessage")
        compare(message.visible, true)
        verify(message.text.indexOf("directory") >= 0)
        verify(message.text.indexOf("not been modified") >= 0)
        compare(find("addField").visible, false)
        compare(find("taskList").visible, false)
    }
}
