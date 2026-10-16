#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "database.h"
#include "deeplink.h"
#include "globalhotkey.h"
#include "selectioncapture.h"
#include "integrationserver.h"
#include "reminderscheduler.h"
#include "shortcutmanager.h"
#include "singleinstance.h"
#include "tasklistmodel.h"
#include "testprofile.h"
#include "taskrepository.h"
#include "widgetcontroller.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TaskNest"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/tasknest-256.png")));
    // Basic is the neutral base for the custom TaskNest controls (see Theme.qml).
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // One TaskNest per database. The lock lives next to the database file, so
    // separate portable copies do not block each other. A second launch brings
    // the running window forward and exits; a stale lock from a crashed
    // instance is detected and replaced (QLockFile checks that the owner is
    // really gone).
    // Optional scratch test profile (--test-profile <folder>): its own data,
    // discovery and registration locations. Parsed first, and an unusable one
    // stops here, before any database or real location is touched. Without the
    // option nothing below differs from a normal launch.
    const TestProfile::Profile testProfile = TestProfile::fromArguments(app.arguments(), /*create=*/true);
    if (testProfile.requested && !testProfile.active) {
        qCritical().noquote() << "Invalid --test-profile:" << testProfile.error;
#ifdef Q_OS_WIN
        // (no dialog for the headless platform automated tests use)
        if (app.platformName() != QLatin1String("offscreen")) {
            const QString message = QStringLiteral("TaskNest was not started.\n\n") + testProfile.error;
            MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(message.utf16()),
                        L"TaskNest test profile", MB_OK | MB_ICONERROR);
        }
#endif
        return 2;
    }
    QString databasePath = testProfile.active ? testProfile.databasePath
                                              : Database::defaultDatabasePath();
    // A tasknest:// link given on the command line (Windows passes the link a
    // user clicked as the single argument of a new launch).
    const QString startupLink = DeepLink::fromArguments(app.arguments());
    SingleInstanceGuard instanceGuard;
    if (!databasePath.isEmpty() && !instanceGuard.acquire(databasePath + QStringLiteral(".lock"))) {
        // Another instance owns the database: hand the link to it (it validates
        // it) and exit. This process opens no database and shows no window.
        bool delivered = false;
        if (!startupLink.isEmpty()) {
            const QByteArray payload(reinterpret_cast<const char *>(startupLink.utf16()),
                                     static_cast<qsizetype>(startupLink.size() * 2));
            delivered = SingleInstanceGuard::sendDataToProcess(
                instanceGuard.ownerPid(), DeepLinkRouter::kCopyDataId, payload);
        }
        if (!delivered || startupLink.isEmpty())
            SingleInstanceGuard::activateWindowOfProcess(instanceGuard.ownerPid());
        return 0;
    }

    // Declared after `app` and in dependency order, so they are destroyed in
    // reverse: model, repository, then the connection.
    Database database;
    // An empty path means "no usable location": initialize() then reports why.
    const bool databaseReady = database.initialize(databasePath);
    TaskRepository repository(database);
    TaskListModel taskModel(repository);
    ReminderScheduler reminders(repository); // destroyed before the model/repository
    // Floating widget backing object: two read-only task views plus the saved
    // widget preferences. Destroyed before the model/repository it uses.
    WidgetController widget(taskModel, repository);
    ShortcutManager shortcuts;
    shortcuts.setTestProfile(testProfile);
    // Off unless the user enabled it in Settings; owns no pipe until then.
    IntegrationServer integration(taskModel, repository, database.databasePath());
    if (testProfile.active)
        integration.setDiscoveryDirectory(testProfile.discoveryDirectory);
    // Links forwarded by a second launch (and the one on our own command line).
    DeepLinkRouter deepLinks;
    app.installNativeEventFilter(&deepLinks);
    // Optional global Quick Add shortcut; unregistered unless the user enabled it.
    GlobalHotkey hotkey(repository);
    // Optional shortcut that reads the selection of another application once,
    // on explicit request (UI Automation; no hook, no clipboard use).
    GlobalHotkey captureHotkey(repository, QStringLiteral("global_capture_selection"),
                               QStringLiteral("Ctrl+Alt+C"),
                               {QStringLiteral("Ctrl+Alt+C"), QStringLiteral("Ctrl+Alt+S"),
                                QStringLiteral("Ctrl+Alt+Shift+C"), QStringLiteral("Ctrl+Alt+Y")});
    SelectionCapture selectionCapture;
    QObject::connect(&captureHotkey, &GlobalHotkey::activated, &selectionCapture,
                     &SelectionCapture::capture);

    QString startupError = databaseReady ? QString() : database.lastError();
    // The cleanup policy is read before the first load, so completed tasks the
    // user asked to be removed are never shown. The default is "never delete".
    if (databaseReady) {
        taskModel.loadCleanupPolicy();
        taskModel.applyCleanup();
    }
    if (databaseReady && !taskModel.refresh())
        startupError = taskModel.lastError();
    if (!startupError.isEmpty())
        qCritical().noquote() << "Startup failed:" << startupError;

    if (startupError.isEmpty()) {
        widget.load();
        widget.refreshViews();
        integration.load();
        hotkey.load();
        captureHotkey.load();
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &integration,
                         &IntegrationServer::stop);

        // Any task change can move the next reminder; so can returning to the
        // app (clock/date changes) and resuming from sleep.
        QObject::connect(&taskModel, &TaskListModel::tasksMutated,
                         &reminders, &ReminderScheduler::reschedule);
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &reminders,
                         [&reminders, &taskModel, &widget](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive) {
                                 // A retention period may have run out while away.
                                 taskModel.applyCleanup();
                                 widget.refreshViews(); // the day may have changed
                                 reminders.reschedule();
                             }
                         });
        reminders.watchSystemResume();
        reminders.reschedule(); // announces overdue reminders, arms the timer
    }

    // Honor Windows "Show animations in Windows" being switched off.
    bool reducedMotion = false;
#ifdef Q_OS_WIN
    BOOL animationsOn = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animationsOn, 0))
        reducedMotion = !animationsOn;
#endif

    QQmlApplicationEngine engine;
    QQmlEngine::setObjectOwnership(&taskModel, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&reminders, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&widget, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&shortcuts, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&integration, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&deepLinks, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&hotkey, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&captureHotkey, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&selectionCapture, QQmlEngine::CppOwnership);
    engine.setInitialProperties({
        {QStringLiteral("shortcutManager"), QVariant::fromValue<QObject *>(&shortcuts)},
        {QStringLiteral("deepLinkRouter"), QVariant::fromValue<QObject *>(&deepLinks)},
        {QStringLiteral("selectionCapture"), QVariant::fromValue<QObject *>(&selectionCapture)},
        {QStringLiteral("captureHotkey"),
         QVariant::fromValue<QObject *>(startupError.isEmpty() ? &captureHotkey : nullptr)},
        {QStringLiteral("globalHotkey"),
         QVariant::fromValue<QObject *>(startupError.isEmpty() ? &hotkey : nullptr)},
        {QStringLiteral("integrationServer"),
         QVariant::fromValue<QObject *>(startupError.isEmpty() ? &integration : nullptr)},
        {QStringLiteral("databaseError"), startupError},
        {QStringLiteral("databasePath"), QDir::toNativeSeparators(database.databasePath())},
        {QStringLiteral("reducedMotion"), reducedMotion},
        {QStringLiteral("widgetController"),
         QVariant::fromValue<QObject *>(startupError.isEmpty() ? &widget : nullptr)},
        {QStringLiteral("reminderScheduler"),
         QVariant::fromValue<QObject *>(startupError.isEmpty() ? &reminders : nullptr)},
        // No model when the database is unusable: no task operations are possible.
        {QStringLiteral("taskModel"),
         QVariant::fromValue<QObject *>(startupError.isEmpty() ? &taskModel : nullptr)},
    });
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("TaskNest", "Main");

    // First launch started by a link: handle it once the window exists.
    if (!startupLink.isEmpty() && startupError.isEmpty())
        QTimer::singleShot(0, &deepLinks, [&deepLinks, startupLink]() { deepLinks.handleUrl(startupLink); });

    return app.exec();
}
