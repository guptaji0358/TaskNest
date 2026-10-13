#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#include "database.h"
#include "tasklistmodel.h"
#include "taskrepository.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TaskNest"));
    QQuickStyle::setStyle(QStringLiteral("Fusion")); // desktop look; follows the system palette

    // Declared after `app` and in dependency order, so they are destroyed in
    // reverse: model, repository, then the connection.
    Database database;
    const bool databaseReady = database.initialize();
    TaskRepository repository(database);
    TaskListModel taskModel(repository);

    QString startupError = databaseReady ? QString() : database.lastError();
    if (databaseReady && !taskModel.refresh())
        startupError = taskModel.lastError();
    if (!startupError.isEmpty())
        qCritical().noquote() << "Startup failed:" << startupError;

    QQmlApplicationEngine engine;
    QQmlEngine::setObjectOwnership(&taskModel, QQmlEngine::CppOwnership);
    engine.setInitialProperties({
        {QStringLiteral("databaseError"), startupError},
        {QStringLiteral("databasePath"), QDir::toNativeSeparators(database.databasePath())},
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
