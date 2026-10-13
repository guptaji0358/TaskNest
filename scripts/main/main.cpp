#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "database.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TaskNest"));

    // Declared after `app` so it is destroyed (connection removed) first.
    Database database;
    const bool databaseReady = database.initialize();
    if (!databaseReady)
        qCritical().noquote() << "Database initialization failed:" << database.lastError();

    QQmlApplicationEngine engine;
    engine.setInitialProperties({
        {QStringLiteral("databaseError"), databaseReady ? QString() : database.lastError()},
        {QStringLiteral("databasePath"), QDir::toNativeSeparators(database.databasePath())},
    });
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("TaskNest", "Main");

    return app.exec();
}
