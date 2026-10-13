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
#include "tasklistmodel.h"
#include "taskrepository.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TaskNest"));
    // Basic is the neutral base for the custom TaskNest controls (see Theme.qml).
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Declared after `app` and in dependency order, so they are destroyed in
    // reverse: model, repository, then the connection.
    Database database;
    const bool databaseReady = database.initialize();
    TaskRepository repository(database);
    TaskListModel taskModel(repository);
    ReminderScheduler reminders(repository); // destroyed before the model/repository

    QString startupError = databaseReady ? QString() : database.lastError();
    if (databaseReady && !taskModel.refresh())
        startupError = taskModel.lastError();
    if (!startupError.isEmpty())
        qCritical().noquote() << "Startup failed:" << startupError;

    if (startupError.isEmpty()) {
        // Any task change can move the next reminder; so can returning to the
        // app (clock/date changes) and resuming from sleep.
        QObject::connect(&taskModel, &TaskListModel::tasksMutated,
                         &reminders, &ReminderScheduler::reschedule);
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &reminders,
                         [&reminders](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive)
                                 reminders.reschedule();
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
    engine.setInitialProperties({
        {QStringLiteral("databaseError"), startupError},
        {QStringLiteral("databasePath"), QDir::toNativeSeparators(database.databasePath())},
        {QStringLiteral("reducedMotion"), reducedMotion},
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
