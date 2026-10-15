#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "database.h"
#include "reminderscheduler.h"
#include "shortcutmanager.h"
#include "singleinstance.h"
#include "tasklistmodel.h"
#include "taskrepository.h"
#include "widgetcontroller.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TaskNest"));
    // Basic is the neutral base for the custom TaskNest controls (see Theme.qml).
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // One TaskNest per database. The lock lives next to the database file, so
    // separate portable copies do not block each other. A second launch brings
    // the running window forward and exits; a stale lock from a crashed
    // instance is detected and replaced (QLockFile checks that the owner is
    // really gone).
    QString databasePath = Database::defaultDatabasePath();
    SingleInstanceGuard instanceGuard;
    if (!databasePath.isEmpty() && !instanceGuard.acquire(databasePath + QStringLiteral(".lock"))) {
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
    engine.setInitialProperties({
        {QStringLiteral("shortcutManager"), QVariant::fromValue<QObject *>(&shortcuts)},
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

    return app.exec();
}
