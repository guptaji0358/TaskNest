// Qt Quick Test host: runs tests/qml/tst_*.qml against the real MainView.qml
// with a TaskListModel on a temporary database (exposed to QML as `fixture`).

#include "database.h"
#include "reminderscheduler.h"
#include "tasklistmodel.h"
#include "taskrepository.h"

#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QtQuickTest>

#include <memory>

class UiFixture : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *model READ model CONSTANT)
    Q_PROPERTY(QObject *scheduler READ scheduler CONSTANT)

public:
    UiFixture()
        : m_database(QStringLiteral("ui_test"))
        , m_repository(m_database)
        , m_model(m_repository)
        , m_scheduler(m_repository)
    {
        QObject::connect(&m_model, &TaskListModel::tasksMutated, &m_scheduler,
                         &ReminderScheduler::reschedule);
        m_database.initialize(m_dir.filePath(QStringLiteral("ui.db")));
        m_model.refresh();
    }

    QObject *model() { return &m_model; }
    QObject *scheduler() { return &m_scheduler; }

    // Deletes every stored task and returns the model to the default view.
    Q_INVOKABLE bool reset()
    {
        m_scheduler.stop(); // forget announced reminders from the previous test
        QList<Task> all;
        if (!m_repository.loadTasks({}, &all))
            return false;
        for (const Task &task : all) {
            if (!m_repository.deleteTask(task.id))
                return false;
        }
        m_model.setSearchText(QString());
        return m_model.setView(TaskListModel::Everything, QString(), TaskListModel::All);
    }

    // What is actually stored, read through the repository (not the model).
    Q_INVOKABLE QVariantList stored()
    {
        QList<Task> all;
        QVariantList result;
        if (!m_repository.loadTasks({}, &all))
            return result;
        for (const Task &task : all) {
            QVariantMap map;
            map[QStringLiteral("id")] = task.id;
            map[QStringLiteral("title")] = task.title;
            map[QStringLiteral("notes")] = task.notes;
            map[QStringLiteral("listName")] = task.listName;
            map[QStringLiteral("priority")] = task.priority;
            map[QStringLiteral("completed")] = task.completed;
            map[QStringLiteral("hasDue")] = task.dueAt.has_value();
            map[QStringLiteral("hasReminder")] = task.reminderAt.has_value();
            map[QStringLiteral("reminderSent")] = task.reminderSent;
            map[QStringLiteral("reminderAt")] =
                task.reminderAt ? QVariant(task.reminderAt->toLocalTime()) : QVariant();
            map[QStringLiteral("dueAt")] = task.dueAt ? QVariant(task.dueAt->toLocalTime()) : QVariant();
            result << map;
        }
        return result;
    }

    // Simulates a lost connection so operations fail.
    Q_INVOKABLE void breakDatabase() { m_database.shutdown(); }

private:
    QTemporaryDir m_dir;
    Database m_database;
    TaskRepository m_repository;
    TaskListModel m_model;
    ReminderScheduler m_scheduler;
};

class Setup : public QObject
{
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        m_fixture.reset(); // release the previous connection name first
        m_fixture = std::make_unique<UiFixture>();
        engine->rootContext()->setContextProperty(QStringLiteral("fixture"), m_fixture.get());
    }

private:
    std::unique_ptr<UiFixture> m_fixture;
};

int main(int argc, char **argv)
{
    qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion"); // same style as the application
    Setup setup;
    return quick_test_main_with_setup(argc, argv, "tasknest_ui", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "ui_main.moc"
