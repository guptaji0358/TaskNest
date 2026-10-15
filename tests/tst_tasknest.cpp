// Tests for the database migration, TaskRepository and TaskListModel.
// Every test works on a database file inside a temporary directory.

#include "cleanuppolicy.h"
#include "database.h"
#include "reminderscheduler.h"
#include "singleinstance.h"
#include "task.h"
#include "tasklistmodel.h"
#include "taskrepository.h"
#include "widgetcontroller.h"

#include <QAbstractEventDispatcher>
#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QSysInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

const QString kSeedConnection = QStringLiteral("tst_seed");

// Runs raw SQL against a file with its own short-lived connection, to create
// legacy databases and to inspect results independently of Database.
bool runSql(const QString &path, const QStringList &statements)
{
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kSeedConnection);
        db.setDatabaseName(path);
        ok = db.open();
        for (const QString &sql : statements) {
            if (!ok)
                break;
            QSqlQuery query(db);
            ok = query.exec(sql);
            if (!ok)
                qWarning() << "seed SQL failed:" << sql << query.lastError().text();
        }
    }
    QSqlDatabase::removeDatabase(kSeedConnection);
    return ok;
}

QVariant scalar(const QString &path, const QString &sql)
{
    QVariant value;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kSeedConnection);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(sql) && query.next())
                value = query.value(0);
        }
    }
    QSqlDatabase::removeDatabase(kSeedConnection);
    return value;
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

QStringList v1Statements()
{
    return {
        QStringLiteral("CREATE TABLE app_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral("INSERT INTO app_metadata VALUES ('schema_version', '1')"),
        QStringLiteral("INSERT INTO app_metadata VALUES ('marker', 'keep me')"),
        QStringLiteral("PRAGMA user_version = 1"),
    };
}

Task draft(const QString &title)
{
    Task task;
    task.title = title;
    return task;
}

// A database + repository + scheduler driven by a fake clock.
struct SchedulerFixture
{
    Database db;
    TaskRepository repo;
    ReminderScheduler scheduler;
    QDateTime now;
    QList<qint64> fired;
    QList<qint64> cancelled;
    QStringList errors;
    bool ok = false;

    SchedulerFixture(const QString &file, const QDateTime &start)
        : db(QStringLiteral("rem_test"))
        , repo(db)
        , scheduler(repo)
        , now(start)
    {
        ok = db.initialize(file);
        scheduler.setClock([this] { return now; });
        QObject::connect(&scheduler, &ReminderScheduler::reminderFired,
                         [this](qint64 id, const QString &, const QDateTime &) { fired << id; });
        QObject::connect(&scheduler, &ReminderScheduler::reminderCancelled,
                         [this](qint64 id) { cancelled << id; });
        QObject::connect(&scheduler, &ReminderScheduler::errorOccurred,
                         [this](const QString &message) { errors << message; });
    }

    // Adds a task whose reminder is `minutesFromNow` after the fake clock.
    qint64 addReminder(const QString &title, int minutesFromNow)
    {
        Task task;
        task.title = title;
        task.reminderAt = now.addSecs(minutesFromNow * 60);
        Task created;
        return repo.addTask(task, &created) ? created.id : 0;
    }
};

const QDateTime kStart(QDate(2030, 6, 1), QTime(12, 0), QTimeZone::utc());

} // namespace

class TestTaskNest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // ---- migration -------------------------------------------------------

    void migration_freshDatabaseIsCurrent()
    {
        const QString file = path("fresh.db");
        {
            Database db;
            QVERIFY2(db.initialize(file), qPrintable(db.lastError()));
            QCOMPARE(db.schemaVersion(), 3);
        }
        QCOMPARE(scalar(file, "PRAGMA user_version").toInt(), 3);
        QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='schema_version'").toString(),
                 QString("3"));
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM sqlite_master WHERE name='tasks'").toInt(), 1);
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 0);
    }

    void migration_v1ToCurrentPreservesMetadataAndOtherTables()
    {
        const QString file = path("v1.db");
        QStringList seed = v1Statements();
        seed << "CREATE TABLE user_notes (n TEXT)" << "INSERT INTO user_notes VALUES ('precious')";
        QVERIFY(runSql(file, seed));

        {
            Database db;
            QVERIFY2(db.initialize(file), qPrintable(db.lastError()));
            QCOMPARE(db.schemaVersion(), 3);
        }
        QCOMPARE(scalar(file, "PRAGMA user_version").toInt(), 3);
        QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='schema_version'").toString(),
                 QString("3"));
        QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='marker'").toString(),
                 QString("keep me"));
        QCOMPARE(scalar(file, "SELECT n FROM user_notes").toString(), QString("precious"));
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM sqlite_master WHERE name='tasks'").toInt(), 1);
    }

    void migration_reopenKeepsData()
    {
        const QString file = path("reopen.db");
        qint64 id = 0;
        {
            Database db;
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            Task created;
            QVERIFY2(repo.addTask(draft("Persist me"), &created), qPrintable(repo.lastError()));
            id = created.id;
        }
        const QByteArray before = fileHash(file);
        {
            Database db;
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            Task loaded;
            QVERIFY2(repo.getTask(id, &loaded), qPrintable(repo.lastError()));
            QCOMPARE(loaded.title, QString("Persist me"));
        }
        // Opening an up-to-date database must not rewrite it.
        QCOMPARE(fileHash(file), before);
    }

    void migration_secondInitializeIsIdempotent()
    {
        Database db;
        QVERIFY(db.initialize(path("twice.db")));
        QVERIFY(db.initialize(path("twice.db")));
        QCOMPARE(db.schemaVersion(), 3);
    }

    void migration_failureRollsBackCompletely()
    {
        const QString file = path("blocked.db");
        QStringList seed = v1Statements();
        // A table named like the v2 index makes the last migration step fail,
        // after the tasks table has already been created in the transaction.
        seed << "CREATE TABLE idx_tasks_due_at (x INTEGER)";
        QVERIFY(runSql(file, seed));

        {
            Database db;
            QVERIFY(!db.initialize(file));
            QVERIFY(!db.isReady());
            QVERIFY(!db.lastError().isEmpty());
            QVERIFY(!QSqlDatabase::contains(db.connectionName()));
        }
        QCOMPARE(scalar(file, "PRAGMA user_version").toInt(), 1);
        QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='schema_version'").toString(),
                 QString("1"));
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM sqlite_master WHERE name='tasks'").toInt(), 0);
        QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='marker'").toString(),
                 QString("keep me"));
    }

    void migration_newerSchemaIsRejectedUntouched()
    {
        const QString file = path("future.db");
        QStringList seed = v1Statements();
        seed << "PRAGMA user_version = 99";
        QVERIFY(runSql(file, seed));
        const QByteArray before = fileHash(file);

        Database db;
        QVERIFY(!db.initialize(file));
        QVERIFY(db.lastError().contains("newer"));
        QCOMPARE(fileHash(file), before);
    }

    void migration_invalidFileIsRejectedUntouched()
    {
        const QString file = path("notadb.db");
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("this is not a database, just somebody's text\n");
        }
        const QByteArray before = fileHash(file);

        Database db;
        QVERIFY(!db.initialize(file));
        QVERIFY(!db.isReady());
        QCOMPARE(fileHash(file), before);
    }

    void migration_upgradeKeepsBackupOfTheOriginal()
    {
        const QString file = path("backup.db");
        QStringList seed = v1Statements();
        seed << "CREATE TABLE user_notes (n TEXT)" << "INSERT INTO user_notes VALUES ('precious')";
        QVERIFY(runSql(file, seed));
        const QByteArray original = fileHash(file);

        {
            Database db;
            QVERIFY2(db.initialize(file), qPrintable(db.lastError()));
            QCOMPARE(db.schemaVersion(), 3);
        }
        const QString backup = file + ".v1.bak";
        QVERIFY(QFile::exists(backup));
        QCOMPARE(fileHash(backup), original);                          // byte-identical
        QCOMPARE(scalar(backup, "PRAGMA user_version").toInt(), 1);    // still the old schema
        QCOMPARE(scalar(file, "PRAGMA user_version").toInt(), 3);

        // a database that is already current is never backed up again
        const QByteArray backupBefore = fileHash(backup);
        { Database db; QVERIFY(db.initialize(file)); }
        QCOMPARE(fileHash(backup), backupBefore);
    }

    void migration_failedBackupBlocksTheUpgrade()
    {
        const QString file = path("nobackup.db");
        QVERIFY(runSql(file, v1Statements()));
        // A directory where the backup file should go makes the copy impossible.
        QVERIFY(QDir().mkpath(file + ".v1.bak"));
        const QByteArray before = fileHash(file);

        Database db;
        QVERIFY(!db.initialize(file));
        QVERIFY(db.lastError().contains("back up"));
        QVERIFY(!db.isReady());
        QCOMPARE(fileHash(file), before);   // untouched: still schema v1
        QCOMPARE(scalar(file, "PRAGMA user_version").toInt(), 1);
    }

    void migration_freshDatabaseNeedsNoBackup()
    {
        const QString file = path("nobackup_fresh.db");
        Database db;
        QVERIFY(db.initialize(file));
        QVERIFY(!QFile::exists(file + ".v0.bak"));
        QVERIFY(!QFile::exists(file + ".v1.bak"));
    }

    // ---- repository ------------------------------------------------------

    void repo_addAssignsIdsAndStoresFields()
    {
        Database db;
        QVERIFY(db.initialize(path("add.db")));
        TaskRepository repo(db);

        Task first;
        Task second;
        QVERIFY2(repo.addTask(draft("  First  "), &first), qPrintable(repo.lastError()));
        Task withFields = draft("Second");
        withFields.notes = "some notes";
        withFields.listName = "Work";
        withFields.priority = 2;
        withFields.dueAt = QDateTime(QDate(2030, 5, 17), QTime(9, 30, 15, 250), QTimeZone::utc());
        QVERIFY2(repo.addTask(withFields, &second), qPrintable(repo.lastError()));

        QVERIFY(first.id > 0);
        QVERIFY(second.id > first.id);
        QCOMPARE(first.title, QString("First")); // trimmed
        QCOMPARE(first.listName, QString("Inbox"));
        QCOMPARE(first.notes, QString());
        QCOMPARE(first.priority, 0);
        QVERIFY(!first.dueAt.has_value());
        QVERIFY(!first.completed);
        QVERIFY(first.createdAt.isValid());
        QCOMPARE(first.createdAt, first.updatedAt);

        QCOMPARE(second.notes, QString("some notes"));
        QCOMPARE(second.listName, QString("Work"));
        QCOMPARE(second.priority, 2);
        QVERIFY(second.dueAt.has_value());
        QCOMPARE(*second.dueAt, *withFields.dueAt);
    }

    void repo_rejectsBlankTitlesAndBadPriority()
    {
        Database db;
        QVERIFY(db.initialize(path("validate.db")));
        TaskRepository repo(db);

        QVERIFY(!repo.addTask(draft("")));
        QVERIFY(repo.lastError().contains("title"));
        QVERIFY(!repo.addTask(draft("   \t  ")));

        Task badPriority = draft("ok");
        badPriority.priority = 3;
        QVERIFY(!repo.addTask(badPriority));
        badPriority.priority = -1;
        QVERIFY(!repo.addTask(badPriority));

        QList<Task> all;
        QVERIFY(repo.loadTasks({}, &all));
        QCOMPARE(all.size(), 0);

        // The schema itself also refuses blank titles.
        Task stored;
        QVERIFY(repo.addTask(draft("keep"), &stored));
        stored.title = "   ";
        QVERIFY(!repo.updateTask(stored));
        Task reloaded;
        QVERIFY(repo.getTask(stored.id, &reloaded));
        QCOMPARE(reloaded.title, QString("keep"));
    }

    void repo_updateChangesEditableFields()
    {
        Database db;
        QVERIFY(db.initialize(path("update.db")));
        TaskRepository repo(db);

        Task created;
        QVERIFY(repo.addTask(draft("Before"), &created));
        QTest::qWait(5); // ensure a distinguishable updated_at

        Task edited = created;
        edited.title = "After";
        edited.notes = "n";
        edited.listName = "Home";
        edited.priority = 1;
        edited.dueAt = QDateTime(QDate(2031, 1, 2), QTime(3, 4, 5), QTimeZone::utc());
        Task updated;
        QVERIFY2(repo.updateTask(edited, &updated), qPrintable(repo.lastError()));

        QCOMPARE(updated.id, created.id);
        QCOMPARE(updated.title, QString("After"));
        QCOMPARE(updated.notes, QString("n"));
        QCOMPARE(updated.listName, QString("Home"));
        QCOMPARE(updated.priority, 1);
        QVERIFY(updated.dueAt.has_value());
        QCOMPARE(updated.createdAt, created.createdAt);
        QVERIFY(updated.updatedAt > created.updatedAt);

        // Clearing the due date stores NULL again.
        edited.dueAt.reset();
        QVERIFY(repo.updateTask(edited, &updated));
        QVERIFY(!updated.dueAt.has_value());
    }

    void repo_completeAndReopen()
    {
        Database db;
        QVERIFY(db.initialize(path("complete.db")));
        TaskRepository repo(db);
        Task created;
        QVERIFY(repo.addTask(draft("Do it"), &created));
        QTest::qWait(5);

        Task done;
        QVERIFY(repo.setCompleted(created.id, true, &done));
        QVERIFY(done.completed);
        QVERIFY(done.updatedAt > created.updatedAt);

        Task reopened;
        QVERIFY(repo.setCompleted(created.id, false, &reopened));
        QVERIFY(!reopened.completed);
    }

    void repo_deleteRemovesTask()
    {
        Database db;
        QVERIFY(db.initialize(path("delete.db")));
        TaskRepository repo(db);
        Task keep;
        Task gone;
        QVERIFY(repo.addTask(draft("keep"), &keep));
        QVERIFY(repo.addTask(draft("gone"), &gone));

        QVERIFY2(repo.deleteTask(gone.id), qPrintable(repo.lastError()));
        QVERIFY(!repo.getTask(gone.id, nullptr));
        QVERIFY(repo.getTask(keep.id, nullptr));
        QVERIFY(!repo.deleteTask(gone.id)); // already deleted
    }

    void repo_unknownIdsAreReportedNotIgnored()
    {
        Database db;
        QVERIFY(db.initialize(path("unknown.db")));
        TaskRepository repo(db);

        Task missing = draft("ghost");
        missing.id = 4242;
        QVERIFY(!repo.getTask(4242, nullptr));
        QVERIFY(repo.lastError().contains("4242"));
        QVERIFY(!repo.updateTask(missing));
        QVERIFY(repo.lastError().contains("not found"));
        QVERIFY(!repo.setCompleted(4242, true));
        QVERIFY(!repo.deleteTask(4242));
        QVERIFY(repo.lastError().contains("not found"));
    }

    void repo_searchBindsValuesAsLiterals()
    {
        Database db;
        const QString file = path("search.db");
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);

        const QString nasty = "Robert'); DROP TABLE tasks;--";
        QVERIFY(repo.addTask(draft(nasty)));
        QVERIFY(repo.addTask(draft("it's a test")));
        QVERIFY(repo.addTask(draft("100% done")));
        QVERIFY(repo.addTask(draft("100x done")));
        QVERIFY(repo.addTask(draft("snake_case")));
        QVERIFY(repo.addTask(draft("snakeXcase")));

        auto titles = [&](const QString &needle) {
            TaskRepository::Query query;
            query.titleContains = needle;
            QList<Task> found;
            const bool ok = repo.loadTasks(query, &found);
            QStringList result;
            for (const Task &task : found)
                result << task.title;
            return ok ? result : QStringList{"<error>"};
        };

        QCOMPARE(titles("DROP TABLE"), QStringList{nasty});
        QCOMPARE(titles("'); DROP"), QStringList{nasty});
        QCOMPARE(titles("it's"), QStringList{"it's a test"});
        QCOMPARE(titles("' OR '1'='1"), QStringList()); // not treated as SQL
        QCOMPARE(titles("100%"), QStringList{"100% done"});   // % is literal
        QCOMPARE(titles("snake_"), QStringList{"snake_case"}); // _ is literal
        QCOMPARE(titles("DONE").size(), 2);                    // case-insensitive
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 6);

        // Stored verbatim.
        QCOMPARE(scalar(file, "SELECT title FROM tasks WHERE id = 1").toString(), nasty);
    }

    void repo_filtersByCompletion()
    {
        Database db;
        QVERIFY(db.initialize(path("filter.db")));
        TaskRepository repo(db);
        Task a, b, c;
        QVERIFY(repo.addTask(draft("a"), &a));
        QVERIFY(repo.addTask(draft("b"), &b));
        QVERIFY(repo.addTask(draft("c"), &c));
        QVERIFY(repo.setCompleted(b.id, true));

        auto idsFor = [&](TaskRepository::CompletionFilter filter) {
            TaskRepository::Query query;
            query.completion = filter;
            QList<Task> found;
            if (!repo.loadTasks(query, &found))
                return QList<qint64>{-1};
            QList<qint64> ids;
            for (const Task &task : found)
                ids << task.id;
            return ids;
        };
        QCOMPARE(idsFor(TaskRepository::CompletionFilter::All), (QList<qint64>{a.id, b.id, c.id}));
        QCOMPARE(idsFor(TaskRepository::CompletionFilter::Active), (QList<qint64>{a.id, c.id}));
        QCOMPARE(idsFor(TaskRepository::CompletionFilter::Completed), (QList<qint64>{b.id}));
    }

    void repo_changingReminderRearmsIt()
    {
        const QString file = path("reminder.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);

        Task withReminder = draft("remind");
        withReminder.reminderAt = QDateTime(QDate(2030, 1, 1), QTime(8, 0), QTimeZone::utc());
        Task created;
        QVERIFY(repo.addTask(withReminder, &created));
        {
            QSqlDatabase conn = db.connection();
            QSqlQuery query(conn);
            QVERIFY(query.exec("UPDATE tasks SET reminder_sent = 1"));
        }

        // Unchanged reminder time: stays marked as sent.
        Task same = created;
        same.title = "renamed";
        Task result;
        QVERIFY(repo.updateTask(same, &result));
        QVERIFY(result.reminderSent);

        // New reminder time: re-armed.
        same.reminderAt = QDateTime(QDate(2030, 1, 2), QTime(8, 0), QTimeZone::utc());
        QVERIFY(repo.updateTask(same, &result));
        QVERIFY(!result.reminderSent);
    }

    void repo_refusesWhenDatabaseNotReady()
    {
        Database db; // never initialized
        TaskRepository repo(db);
        QList<Task> tasks;
        QVERIFY(!repo.loadTasks({}, &tasks));
        QVERIFY(!repo.addTask(draft("x")));
        QVERIFY(repo.lastError().contains("not available"));
    }

    // ---- model -----------------------------------------------------------

    void model_rowCountAndRoles()
    {
        Database db;
        QVERIFY(db.initialize(path("model_roles.db")));
        TaskRepository repo(db);
        Task t = draft("Write report");
        t.notes = "quarterly";
        t.listName = "Work";
        t.priority = 2;
        t.dueAt = QDateTime(QDate(2030, 6, 1), QTime(12, 0), QTimeZone::utc());
        Task stored;
        QVERIFY(repo.addTask(t, &stored));
        QVERIFY(repo.addTask(draft("Second")));

        TaskListModel model(repo);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.refresh());

        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.property("count").toInt(), 2);
        const QModelIndex row0 = model.index(0);
        QCOMPARE(model.data(row0, TaskListModel::IdRole).toLongLong(), stored.id);
        QCOMPARE(model.data(row0, TaskListModel::TitleRole).toString(), QString("Write report"));
        QCOMPARE(model.data(row0, TaskListModel::NotesRole).toString(), QString("quarterly"));
        QCOMPARE(model.data(row0, TaskListModel::ListNameRole).toString(), QString("Work"));
        QCOMPARE(model.data(row0, TaskListModel::PriorityRole).toInt(), 2);
        QCOMPARE(model.data(row0, TaskListModel::DueAtRole).toDateTime().toUTC(), *t.dueAt);
        QCOMPARE(model.data(row0, TaskListModel::CompletedRole).toBool(), false);
        QVERIFY(!model.data(model.index(1), TaskListModel::DueAtRole).isValid());

        const QHash<int, QByteArray> names = model.roleNames();
        QCOMPARE(names.value(TaskListModel::TitleRole), QByteArray("title"));
        QCOMPARE(names.value(TaskListModel::CompletedRole), QByteArray("completed"));
    }

    void model_addEmitsInsertAndPersists()
    {
        Database db;
        const QString file = path("model_add.db");
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.refresh());

        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy counts(&model, &TaskListModel::countChanged);

        QVERIFY(model.addTask("One"));
        QVERIFY(model.addTask("Two", "notes", "Work", 0)); // same priority/no due date: appended

        QCOMPARE(inserted.size(), 2);
        QCOMPARE(inserted.at(1).at(1).toInt(), 1); // first == last == 1
        QCOMPARE(inserted.at(1).at(2).toInt(), 1);
        QCOMPARE(resets.size(), 0); // incremental, not a rebuild
        QCOMPARE(counts.size(), 2);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 2);
    }

    void model_failedAddLeavesModelUnchanged()
    {
        Database db;
        QVERIFY(db.initialize(path("model_fail.db")));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.refresh());
        QVERIFY(model.addTask("keep"));

        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QVERIFY(!model.addTask("   "));
        QCOMPARE(errors.size(), 1);
        QCOMPARE(inserted.size(), 0);
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(!model.lastError().isEmpty());
    }

    void model_completeUsesDataChangedAndFilterRemovesRow()
    {
        Database db;
        QVERIFY(db.initialize(path("model_complete.db")));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.refresh());
        QVERIFY(model.addTask("a"));
        QVERIFY(model.addTask("b"));
        const qint64 idA = model.data(model.index(0), TaskListModel::IdRole).toLongLong();

        {
            QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
            QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
            QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
            QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
            QVERIFY(model.setCompleted(idA, true));
            QCOMPARE(changed.size(), 1);
            QCOMPARE(moved.size(), 1);   // finished tasks sink below the open ones
            QCOMPARE(resets.size(), 0);
            QCOMPARE(removed.size(), 0);
            QVERIFY(!model.data(model.index(0), TaskListModel::CompletedRole).toBool()); // "b"
            QVERIFY(model.data(model.index(1), TaskListModel::CompletedRole).toBool());  // "a"
            QCOMPARE(model.data(model.index(1), TaskListModel::IdRole).toLongLong(), idA);
        }

        // With "active only" shown, completing a task removes its row.
        model.setCompletionFilter(TaskListModel::Active);
        QCOMPARE(model.rowCount(), 1);
        const qint64 idB = model.data(model.index(0), TaskListModel::IdRole).toLongLong();
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QVERIFY(model.setCompleted(idB, true));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(model.rowCount(), 0);

        // Re-opening it while viewing "completed only" removes it from there too.
        model.setCompletionFilter(TaskListModel::Completed);
        QCOMPARE(model.rowCount(), 2);
        QVERIFY(model.setCompleted(idB, false));
        QCOMPARE(model.rowCount(), 1);
    }

    void model_removeEmitsRemoveAndPersists()
    {
        Database db;
        const QString file = path("model_remove.db");
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.refresh());
        QVERIFY(model.addTask("a"));
        QVERIFY(model.addTask("b"));
        QVERIFY(model.addTask("c"));
        const qint64 idB = model.data(model.index(1), TaskListModel::IdRole).toLongLong();

        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QVERIFY(model.removeTask(idB));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.at(0).at(1).toInt(), 1);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(1), TaskListModel::TitleRole).toString(), QString("c"));
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 2);

        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QVERIFY(!model.removeTask(idB)); // already gone
        QCOMPARE(errors.size(), 1);
        QCOMPARE(model.rowCount(), 2);
    }

    void model_updateAndSearch()
    {
        Database db;
        QVERIFY(db.initialize(path("model_search.db")));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.refresh());
        QVERIFY(model.addTask("Buy milk"));
        QVERIFY(model.addTask("Call mom"));
        const qint64 idMilk = model.data(model.index(0), TaskListModel::IdRole).toLongLong();

        QVERIFY(model.updateTask(idMilk, "Buy oat milk", "2 cartons", "Inbox", 1));
        QCOMPARE(model.data(model.index(0), TaskListModel::TitleRole).toString(), QString("Buy oat milk"));
        QCOMPARE(model.data(model.index(0), TaskListModel::PriorityRole).toInt(), 1);

        model.setSearchText("milk");
        QCOMPARE(model.rowCount(), 1);
        // A new task that does not match the search is stored but not shown.
        QVERIFY(model.addTask("Water plants"));
        QCOMPARE(model.rowCount(), 1);
        model.setSearchText(QString());
        QCOMPARE(model.rowCount(), 3);

        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QVERIFY(!model.updateTask(9999, "x", "", "Inbox", 0));
        QCOMPARE(errors.size(), 1);
    }

    void model_failedRefreshKeepsRows()
    {
        Database db;
        QVERIFY(db.initialize(path("model_refresh.db")));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.refresh());
        QVERIFY(model.addTask("still here"));

        db.shutdown(); // connection gone: loads must fail loudly
        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QVERIFY(!model.refresh());
        QCOMPARE(errors.size(), 1);
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(!model.addTask("nope"));
    }

    // ---- reminders: storage and editing ----------------------------------

    void reminder_storedAsUtcAndShownAsLocal()
    {
        const QString file = path("rem_utc.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.refresh());

        // 14:30 at UTC+05:30 is 09:00 UTC.
        const QDateTime offset(QDate(2030, 3, 4), QTime(14, 30),
                               QTimeZone::fromSecondsAheadOfUtc(5 * 3600 + 1800));
        Task task = draft("with reminder");
        task.reminderAt = offset;
        Task created;
        QVERIFY(repo.addTask(task, &created));

        QCOMPARE(scalar(file, "SELECT reminder_at FROM tasks").toString(),
                 QString("2030-03-04T09:00:00.000Z"));
        QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks").toInt(), 0);

        QVERIFY(model.refresh());
        const QVariant shown = model.data(model.index(0), TaskListModel::ReminderAtRole);
        QVERIFY(shown.isValid());
        QCOMPARE(shown.toDateTime().timeSpec(), Qt::LocalTime);
        QCOMPARE(shown.toDateTime().toUTC(), offset.toUTC());
        QCOMPARE(shown.toDateTime().toMSecsSinceEpoch(), offset.toMSecsSinceEpoch());
    }

    void reminder_updateClearAndPreserve()
    {
        const QString file = path("rem_edit.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.refresh());

        const QDateTime first(QDate(2030, 1, 1), QTime(8, 0), QTimeZone::utc());
        const QDateTime second(QDate(2030, 1, 2), QTime(8, 0), QTimeZone::utc());
        QVERIFY(model.addTask("t"));
        const qint64 id = model.data(model.index(0), TaskListModel::IdRole).toLongLong();

        // set
        QVERIFY(model.updateTaskWithReminder(id, "t", "", "Inbox", 0, QDateTime(), first));
        QCOMPARE(scalar(file, "SELECT reminder_at FROM tasks").toString(),
                 QString("2030-01-01T08:00:00.000Z"));

        // pretend it was delivered, then edit unrelated fields: reminder and sent flag stay
        QVERIFY(repo.markReminderSent(id));
        QVERIFY(model.updateTask(id, "renamed", "notes", "Work", 2));
        QCOMPARE(scalar(file, "SELECT reminder_at FROM tasks").toString(),
                 QString("2030-01-01T08:00:00.000Z"));
        QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks").toInt(), 1);

        // same reminder through the full update: still sent
        QVERIFY(model.updateTaskWithReminder(id, "renamed", "notes", "Work", 2, QDateTime(), first));
        QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks").toInt(), 1);

        // new time: re-armed
        QVERIFY(model.updateTaskWithReminder(id, "renamed", "notes", "Work", 2, QDateTime(), second));
        QCOMPARE(scalar(file, "SELECT reminder_at FROM tasks").toString(),
                 QString("2030-01-02T08:00:00.000Z"));
        QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks").toInt(), 0);

        // clear
        QVERIFY(model.updateTaskWithReminder(id, "renamed", "notes", "Work", 2, QDateTime(), QDateTime()));
        QVERIFY(scalar(file, "SELECT reminder_at FROM tasks").isNull());
        QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks").toInt(), 0);
    }

    // ---- reminders: scheduler ----------------------------------------------

    void scheduler_choosesNearestPendingReminder()
    {
        SchedulerFixture f(path("sched_nearest.db"), kStart);
        QVERIFY(f.ok);
        f.addReminder("later", 90);
        const qint64 soon = f.addReminder("soon", 30);
        f.addReminder("latest", 600);
        QVERIFY(soon > 0);

        f.scheduler.reschedule();
        QVERIFY(f.scheduler.isTimerActive());
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(30 * 60));
        QCOMPARE(f.scheduler.timerInterval(), 30 * 60 * 1000);
        QCOMPARE(f.fired.size(), 0);
    }

    void scheduler_ignoresCompletedAndSent()
    {
        SchedulerFixture f(path("sched_ignore.db"), kStart);
        QVERIFY(f.ok);
        const qint64 done = f.addReminder("done", 10);
        const qint64 sent = f.addReminder("sent", 20);
        f.addReminder("pending", 40);
        QVERIFY(f.repo.setCompleted(done, true));
        QVERIFY(f.repo.markReminderSent(sent));

        f.scheduler.reschedule();
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(40 * 60));

        // and nothing pending at all: no timer
        f.now = kStart.addSecs(3600 * 24);
        f.scheduler.processDue();
        QCOMPARE(f.fired.size(), 1);
        QVERIFY(f.scheduler.acknowledge(f.fired.first()));
        QVERIFY(!f.scheduler.isTimerActive());
        QVERIFY(!f.scheduler.nextReminderAt().isValid());
    }

    void scheduler_reschedulesAfterTaskMutations()
    {
        SchedulerFixture f(path("sched_mutate.db"), kStart);
        QVERIFY(f.ok);
        TaskListModel model(f.repo);
        QObject::connect(&model, &TaskListModel::tasksMutated, &f.scheduler, &ReminderScheduler::reschedule);
        QVERIFY(model.refresh());

        Task farTask = draft("far");
        farTask.reminderAt = kStart.addSecs(120 * 60);
        QVERIFY(f.repo.addTask(farTask));
        QVERIFY(model.refresh());
        f.scheduler.reschedule();
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(120 * 60));

        // add: earlier reminder takes over
        QVERIFY(model.addTask("near"));
        const qint64 nearId = model.data(model.index(1), TaskListModel::IdRole).toLongLong();
        QVERIFY(model.updateTaskWithReminder(nearId, "near", "", "Inbox", 0, QDateTime(),
                                             kStart.addSecs(15 * 60)));
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(15 * 60));

        // change its time
        QVERIFY(model.updateTaskWithReminder(nearId, "near", "", "Inbox", 0, QDateTime(),
                                             kStart.addSecs(200 * 60)));
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(120 * 60));

        // complete the "far" one: the nearer remaining one is next
        const qint64 farId = model.data(model.index(0), TaskListModel::IdRole).toLongLong();
        QVERIFY(model.setCompleted(farId, true));
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(200 * 60));

        // delete it: nothing left to schedule
        QVERIFY(model.removeTask(nearId));
        QVERIFY(!f.scheduler.isTimerActive());
    }

    void scheduler_announcesOverdueRemindersOldestFirstOnce()
    {
        SchedulerFixture f(path("sched_overdue.db"), kStart);
        QVERIFY(f.ok);
        const qint64 newer = f.addReminder("newer", -10);
        const qint64 older = f.addReminder("older", -60 * 24 * 30);
        const qint64 future = f.addReminder("future", 50);

        f.scheduler.reschedule();
        QVERIFY(f.scheduler.isTimerActive());
        QCOMPARE(f.scheduler.timerInterval(), 0); // due now: fire on the next loop turn

        f.scheduler.processDue();
        QCOMPARE(f.fired, (QList<qint64>{older, newer}));
        QVERIFY(!f.fired.contains(future));
        // next scheduled is the future one; the in-flight ones are not re-armed
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(50 * 60));
        QCOMPARE(f.scheduler.timerInterval(), 50 * 60 * 1000);
    }

    void scheduler_neverAnnouncesTheSameReminderTwice()
    {
        const QString file = path("sched_dupes.db");
        qint64 id = 0;
        {
            SchedulerFixture f(file, kStart);
            QVERIFY(f.ok);
            id = f.addReminder("once", -5);

            f.scheduler.reschedule();
            f.scheduler.processDue();
            f.scheduler.processDue();      // delayed/duplicate timer
            f.scheduler.reschedule();      // refresh / app re-activated
            f.scheduler.reschedule();
            f.scheduler.processDue();
            QCOMPARE(f.fired, QList<qint64>{id});
            QCOMPARE(f.scheduler.inFlightIds(), QList<qint64>{id});

            QVERIFY(f.scheduler.acknowledge(id));
            QVERIFY(f.scheduler.acknowledge(id)); // harmless repeat
            f.scheduler.processDue();
            f.scheduler.reschedule();
            QCOMPARE(f.fired.size(), 1);
            QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks").toInt(), 1);
        }
        // restart after acknowledgement: not announced again
        SchedulerFixture again(file, kStart.addSecs(3600));
        QVERIFY(again.ok);
        again.scheduler.reschedule();
        again.scheduler.processDue();
        QCOMPARE(again.fired.size(), 0);
    }

    void scheduler_unacknowledgedReminderSurvivesRestart()
    {
        const QString file = path("sched_restart.db");
        qint64 id = 0;
        {
            SchedulerFixture f(file, kStart);
            QVERIFY(f.ok);
            id = f.addReminder("persist", 20);
            f.addReminder("also", 45);
            f.scheduler.reschedule();
            f.now = kStart.addSecs(25 * 60);
            f.scheduler.processDue();
            QCOMPARE(f.fired, QList<qint64>{id}); // shown, but app closes before it is handled
        }
        SchedulerFixture again(file, kStart.addSecs(30 * 60));
        QVERIFY(again.ok);
        again.scheduler.reschedule();
        QCOMPARE(again.scheduler.timerInterval(), 0); // still pending: announced again
        again.scheduler.processDue();
        QCOMPARE(again.fired, QList<qint64>{id});
        QCOMPARE(again.scheduler.nextReminderAt(), kStart.addSecs(45 * 60));
    }

    void scheduler_completingAnAnnouncedTaskCancelsItsReminder()
    {
        SchedulerFixture f(path("sched_cancel.db"), kStart);
        QVERIFY(f.ok);
        const qint64 id = f.addReminder("cancel me", -1);
        f.scheduler.reschedule();
        f.scheduler.processDue();
        QCOMPARE(f.fired, QList<qint64>{id});

        QVERIFY(f.repo.setCompleted(id, true));
        f.scheduler.reschedule();
        QCOMPARE(f.cancelled, QList<qint64>{id});
        QVERIFY(f.scheduler.inFlightIds().isEmpty());

        // re-opening the task must not replay a reminder the user already acted on
        QVERIFY(f.repo.setCompleted(id, false));
        f.scheduler.reschedule();
        f.scheduler.processDue();
        QCOMPARE(f.fired.size(), 1);
    }

    void scheduler_changedReminderTimeCancelsAnnouncement()
    {
        SchedulerFixture f(path("sched_changed.db"), kStart);
        QVERIFY(f.ok);
        const qint64 id = f.addReminder("moved", -1);
        f.scheduler.reschedule();
        f.scheduler.processDue();
        QCOMPARE(f.fired.size(), 1);

        Task task;
        QVERIFY(f.repo.getTask(id, &task));
        task.reminderAt = kStart.addSecs(3 * 3600);
        QVERIFY(f.repo.updateTask(task));
        f.scheduler.reschedule();
        QCOMPARE(f.cancelled, QList<qint64>{id});
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(3 * 3600));
    }

    void scheduler_databaseErrorsNeverMarkAsSent()
    {
        const QString file = path("sched_error.db");
        SchedulerFixture f(file, kStart);
        QVERIFY(f.ok);
        const qint64 id = f.addReminder("fragile", -1);
        f.scheduler.reschedule();
        f.scheduler.processDue();
        QCOMPARE(f.fired, QList<qint64>{id});

        f.db.shutdown();                       // connection lost
        QVERIFY(!f.scheduler.acknowledge(id)); // must fail ...
        QVERIFY(!f.errors.isEmpty());
        QCOMPARE(f.scheduler.inFlightIds(), QList<qint64>{id}); // ... and stay pending

        // while the database is down, scheduling backs off instead of spinning
        f.errors.clear();
        f.scheduler.processDue();
        QVERIFY(!f.errors.isEmpty());
        QVERIFY(f.scheduler.isTimerActive());
        QVERIFY(f.scheduler.timerInterval() >= 1000);

        QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks").toInt(), 0);

        // recovery: reopen and the reminder is still there
        QVERIFY(f.db.initialize(file));
        QVERIFY(f.scheduler.acknowledge(id));
        QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks").toInt(), 1);
    }

    void scheduler_usesOneSingleShotTimerAndNeverPolls()
    {
        SchedulerFixture f(path("sched_poll.db"), kStart);
        QVERIFY(f.ok);
        f.scheduler.reschedule();
        QVERIFY(!f.scheduler.isTimerActive()); // nothing pending: nothing runs

        f.addReminder("tomorrow", 24 * 60);
        f.scheduler.reschedule();
        QVERIFY(f.scheduler.isTimerActive());
        // armed once for the whole wait, not a short repeating tick
        QCOMPARE(f.scheduler.timerInterval(), 24 * 60 * 60 * 1000);
        const QList<QTimer *> timers = f.scheduler.findChildren<QTimer *>();
        QVERIFY(timers.isEmpty()); // the only timer is the member, which is single-shot

        QTest::qWait(150);
        QCOMPARE(f.fired.size(), 0);
        QCOMPARE(f.scheduler.timerInterval(), 24 * 60 * 60 * 1000);

        f.scheduler.stop();
        QVERIFY(!f.scheduler.isTimerActive());
    }

    void scheduler_veryDistantReminderIsClampedAndReArms()
    {
        SchedulerFixture f(path("sched_far.db"), kStart);
        QVERIFY(f.ok);
        // ~70 years away: further than a QTimer interval (int ms, ~24.8 days) can hold.
        Task task = draft("someday");
        task.reminderAt = kStart.addYears(70);
        QVERIFY(f.repo.addTask(task));

        f.scheduler.reschedule();
        QVERIFY(f.scheduler.isTimerActive());
        QCOMPARE(f.scheduler.timerInterval(), std::numeric_limits<int>::max());
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addYears(70));

        // The timer fires early (as it must, after ~24.8 days): nothing is announced
        // and the wait is simply armed again.
        f.now = kStart.addDays(25);
        f.scheduler.processDue();
        QCOMPARE(f.fired.size(), 0);
        QVERIFY(f.scheduler.isTimerActive());
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addYears(70));
    }

    // ---- quick add, undo delete, single instance ---------------------------------------

    void model_quickAddPersistsEveryDetailAndSchedulesTheReminder()
    {
        const QString file = path("quick.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        ReminderScheduler scheduler(repo);
        scheduler.setClock([] { return kStart; });
        QObject::connect(&model, &TaskListModel::tasksMutated, &scheduler, &ReminderScheduler::reschedule);
        QVERIFY(model.refresh());

        // title only
        QVERIFY(model.addTask("just a title"));
        QCOMPARE(scalar(file, "SELECT list_name FROM tasks WHERE title='just a title'").toString(), QString("Inbox"));
        QVERIFY(!scheduler.isTimerActive());

        // everything at once
        const QDateTime due = kStart.addDays(3);
        const QDateTime reminder = kStart.addSecs(90 * 60);
        QVERIFY(model.addTask("with details", "two\nlines", "Work", 2, due, reminder));
        QCOMPARE(scalar(file, "SELECT notes FROM tasks WHERE title='with details'").toString(), QString("two\nlines"));
        QCOMPARE(scalar(file, "SELECT list_name FROM tasks WHERE title='with details'").toString(), QString("Work"));
        QCOMPARE(scalar(file, "SELECT priority FROM tasks WHERE title='with details'").toInt(), 2);
        QCOMPARE(scalar(file, "SELECT due_at FROM tasks WHERE title='with details'").toString(),
                 due.toUTC().toString(Qt::ISODateWithMs));
        QCOMPARE(scalar(file, "SELECT reminder_at FROM tasks WHERE title='with details'").toString(),
                 reminder.toUTC().toString(Qt::ISODateWithMs));
        QCOMPARE(scalar(file, "SELECT reminder_sent FROM tasks WHERE title='with details'").toInt(), 0);

        // the reminder was scheduled by the add itself
        QVERIFY(scheduler.isTimerActive());
        QCOMPARE(scheduler.nextReminderAt(), reminder);

        // invalid input stores nothing
        QVERIFY(!model.addTask("   ", "notes", "Work", 1, due, reminder));
        QVERIFY(!model.addTask("bad priority", "", "", 7));
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 2);
    }

    void repo_restoreTaskBringsBackTheSameRow()
    {
        const QString file = path("restore.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);

        Task task = draft("Restore me");
        task.notes = "multi\nline";
        task.listName = "Work";
        task.priority = 2;
        task.dueAt = QDateTime(QDate(2031, 3, 4), QTime(5, 6), QTimeZone::utc());
        task.reminderAt = QDateTime(QDate(2031, 3, 4), QTime(4, 0), QTimeZone::utc());
        Task created;
        QVERIFY(repo.addTask(task, &created));
        QVERIFY(repo.setCompleted(created.id, true, &created));
        QVERIFY(repo.markReminderSent(created.id));
        Task before;
        QVERIFY(repo.getTask(created.id, &before));

        QVERIFY(repo.deleteTask(created.id));
        QVERIFY(!repo.getTask(created.id, nullptr));

        Task restored;
        QVERIFY2(repo.restoreTask(before, &restored), qPrintable(repo.lastError()));
        QCOMPARE(restored.id, before.id);
        QCOMPARE(restored.title, before.title);
        QCOMPARE(restored.notes, before.notes);
        QCOMPARE(restored.listName, before.listName);
        QCOMPARE(restored.priority, before.priority);
        QCOMPARE(restored.dueAt, before.dueAt);
        QCOMPARE(restored.reminderAt, before.reminderAt);
        QCOMPARE(restored.completed, true);
        QCOMPARE(restored.reminderSent, true);          // the reminder state is preserved too
        QCOMPARE(restored.createdAt, before.createdAt);
        QCOMPARE(restored.updatedAt, before.updatedAt);

        // a second restore of the same id is refused and changes nothing
        QVERIFY(!repo.restoreTask(before));
        QVERIFY(repo.lastError().contains("cannot be restored"));
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 1);

        // new tasks never reuse the restored id
        Task next;
        QVERIFY(repo.addTask(draft("next"), &next));
        QVERIFY(next.id > before.id);
    }

    void model_undoDeleteRestoresTaskAndReminder()
    {
        const QString file = path("undo.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        ReminderScheduler scheduler(repo);
        scheduler.setClock([] { return kStart; });
        QObject::connect(&model, &TaskListModel::tasksMutated, &scheduler, &ReminderScheduler::reschedule);
        QVERIFY(model.refresh());

        const QDateTime due = kStart.addDays(2);
        const QDateTime reminder = kStart.addSecs(45 * 60);
        QVERIFY(model.addTask("keep order 1"));
        QVERIFY(model.addTask("precious", "notes here", "Work", 2, due, reminder));
        QVERIFY(model.addTask("keep order 2"));
        QCOMPARE(scheduler.nextReminderAt(), reminder);
        qint64 id = 0;
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row), TaskListModel::TitleRole).toString() == "precious")
                id = model.data(model.index(row), TaskListModel::IdRole).toLongLong();
        }
        QVERIFY(id > 0);
        QVERIFY(!model.canUndo());

        QSignalSpy undoChanged(&model, &TaskListModel::undoChanged);
        QVERIFY(model.removeTask(id));
        QVERIFY(model.canUndo());
        QCOMPARE(model.undoTitle(), QString("precious"));
        QVERIFY(undoChanged.size() >= 1);
        QCOMPARE(model.rowCount(), 2);
        // deleted: its reminder is no longer scheduled
        QVERIFY(!scheduler.isTimerActive());
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks WHERE id=" + QString::number(id)).toInt(), 0);

        QVERIFY2(model.undoDelete(), qPrintable(model.lastError()));
        QVERIFY(!model.canUndo());
        QCOMPARE(model.rowCount(), 3);

        // same id, same fields, back in its sorted place (due date first)
        QCOMPARE(model.data(model.index(0), TaskListModel::IdRole).toLongLong(), id);
        QCOMPARE(model.data(model.index(0), TaskListModel::TitleRole).toString(), QString("precious"));
        QCOMPARE(model.data(model.index(0), TaskListModel::NotesRole).toString(), QString("notes here"));
        QCOMPARE(model.data(model.index(0), TaskListModel::ListNameRole).toString(), QString("Work"));
        QCOMPARE(model.data(model.index(0), TaskListModel::PriorityRole).toInt(), 2);
        QCOMPARE(model.data(model.index(0), TaskListModel::DueAtRole).toDateTime().toUTC(), due);
        QCOMPARE(model.data(model.index(0), TaskListModel::ReminderAtRole).toDateTime().toUTC(), reminder);
        QCOMPARE(model.data(model.index(0), TaskListModel::ReminderSentRole).toBool(), false);

        // and the scheduler is armed for it again
        QVERIFY(scheduler.isTimerActive());
        QCOMPARE(scheduler.nextReminderAt(), reminder);

        // a second Undo has nothing to do and says so; nothing is duplicated
        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QVERIFY(!model.undoDelete());
        QCOMPARE(errors.size(), 1);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 3);
    }

    void model_undoAppliesOnlyToTheLatestDeleteAndCanBeDiscarded()
    {
        Database db;
        QVERIFY(db.initialize(path("undo_latest.db")));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.refresh());
        QVERIFY(model.addTask("first"));
        QVERIFY(model.addTask("second"));
        const qint64 firstId = model.data(model.index(0), TaskListModel::IdRole).toLongLong();
        const qint64 secondId = model.data(model.index(1), TaskListModel::IdRole).toLongLong();

        QVERIFY(model.removeTask(firstId));
        QVERIFY(model.removeTask(secondId));
        QCOMPARE(model.undoTitle(), QString("second"));
        QVERIFY(model.undoDelete());
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), TaskListModel::TitleRole).toString(), QString("second"));
        QVERIFY(!model.canUndo()); // "first" is no longer recoverable: bounded to one

        QVERIFY(model.removeTask(secondId));
        QVERIFY(model.canUndo());
        model.discardUndo();
        QVERIFY(!model.canUndo());
        QVERIFY(!model.undoDelete());
    }

    void model_failedUndoKeepsTheSnapshotForARetry()
    {
        const QString file = path("undo_fail.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.refresh());
        QVERIFY(model.addTask("retry me", "n", "Work", 1));
        const qint64 id = model.data(model.index(0), TaskListModel::IdRole).toLongLong();
        QVERIFY(model.removeTask(id));

        db.shutdown(); // restoring is impossible right now
        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QVERIFY(!model.undoDelete());
        QCOMPARE(errors.size(), 1);
        QVERIFY(model.canUndo());          // still recoverable
        QCOMPARE(model.rowCount(), 0);

        QVERIFY(db.initialize(file));
        QVERIFY2(model.undoDelete(), qPrintable(model.lastError()));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), TaskListModel::NotesRole).toString(), QString("n"));
    }

    void model_undoIsRefusedWhenTheIdIsTakenAndNothingIsLost()
    {
        const QString file = path("undo_taken.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.refresh());
        QVERIFY(model.addTask("original"));
        const qint64 id = model.data(model.index(0), TaskListModel::IdRole).toLongLong();
        QVERIFY(model.removeTask(id));

        // something else now occupies that id (e.g. the file was swapped under us)
        QVERIFY(runSql(file, {QString("INSERT INTO tasks (id, title, created_at, updated_at) "
                                      "VALUES (%1, 'squatter', '2030-01-01T00:00:00.000Z', '2030-01-01T00:00:00.000Z')")
                                  .arg(id)}));
        QVERIFY(!model.undoDelete());
        QVERIFY(model.lastError().contains("cannot be restored"));
        QCOMPARE(scalar(file, "SELECT title FROM tasks WHERE id=" + QString::number(id)).toString(), QString("squatter"));
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 1);
    }

    void instance_secondLaunchIsRejectedAndLockIsReleasedOnExit()
    {
        const QString lockPath = path("instance.db.lock");
        SingleInstanceGuard first;
        QVERIFY(first.acquire(lockPath));
        QVERIFY(first.isPrimary());

        SingleInstanceGuard second;
        QVERIFY(!second.acquire(lockPath));          // a competing launch is refused ...
        QVERIFY(!second.isPrimary());
        QCOMPARE(second.ownerPid(), qint64(QCoreApplication::applicationPid())); // ... and knows who owns it

        SingleInstanceGuard third;
        QVERIFY(!third.acquire(lockPath));           // simultaneous attempts all lose

        first.release();                             // the owner exits
        SingleInstanceGuard fourth;
        QVERIFY(fourth.acquire(lockPath));           // the next launch is allowed
    }

    void instance_staleLockFromACrashedProcessDoesNotBlock()
    {
        const QString lockPath = path("stale.db.lock");
        {
            // What an abandoned lock looks like: a pid that is not running, written long ago.
            QFile lock(lockPath);
            QVERIFY(lock.open(QIODevice::WriteOnly));
            lock.write(QByteArray("3999999\nTaskNest\n") + QSysInfo::machineHostName().toUtf8() + "\n");
            lock.close();
            QVERIFY(lock.open(QIODevice::ReadWrite));
            QVERIFY(lock.setFileTime(QDateTime::currentDateTime().addSecs(-600),
                                     QFileDevice::FileModificationTime));
        }
        SingleInstanceGuard guard;
        QVERIFY2(guard.acquire(lockPath), "a dead owner's lock must be treated as stale");
        QVERIFY(guard.isPrimary());

        // and a live owner's lock is never mistaken for stale, however old the file
        SingleInstanceGuard other;
        QVERIFY(!other.acquire(lockPath));
    }

    void instance_unusableLockLocationDoesNotLockTheUserOut()
    {
        SingleInstanceGuard guard;
        // The folder does not exist, so no lock can be created at all.
        QVERIFY(guard.acquire(path("no/such/folder/x.lock")));
        QVERIFY(!guard.lockError().isEmpty());
    }

    void scheduler_realTimerFiresOnceAndStops()
    {
        // Uses the real clock and a real timer, ~0.25 s.
        const QString file = path("sched_real.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        ReminderScheduler scheduler(repo);
        QSignalSpy fired(&scheduler, &ReminderScheduler::reminderFired);

        Task task = draft("real");
        task.reminderAt = QDateTime::currentDateTimeUtc().addMSecs(250);
        QVERIFY(repo.addTask(task));
        scheduler.reschedule();
        QVERIFY(scheduler.isTimerActive());

        QVERIFY(fired.wait(3000));
        QCOMPARE(fired.size(), 1);
        QVERIFY(!scheduler.isTimerActive()); // in flight, nothing else pending
        QTest::qWait(100);
        QCOMPARE(fired.size(), 1);
    }

#ifdef Q_OS_WIN
    void scheduler_systemResumeTriggersReschedule()
    {
        SchedulerFixture f(path("sched_resume.db"), kStart);
        QVERIFY(f.ok);
        f.addReminder("slept through", 30);
        f.scheduler.watchSystemResume();
        f.scheduler.reschedule();
        QCOMPARE(f.scheduler.timerInterval(), 30 * 60 * 1000);

        // The computer slept for two hours; the clock jumped, the timer did not.
        f.now = kStart.addSecs(2 * 3600);

        MSG msg = {};
        msg.message = WM_POWERBROADCAST;
        msg.wParam = PBT_APMRESUMEAUTOMATIC;
        qintptr result = 0;
        QAbstractEventDispatcher::instance()->filterNativeEvent("windows_generic_MSG", &msg, &result);

        QCOMPARE(f.scheduler.timerInterval(), 0); // recalculated: overdue, fire now
        f.scheduler.processDue();
        QCOMPARE(f.fired.size(), 1);
    }
#endif

    void repo_searchIsCaseInsensitiveBeyondAscii()
    {
        Database db;
        QVERIFY(db.initialize(path("unicode.db")));
        TaskRepository repo(db);

        // U+00DC/U+00FC (U with diaeresis), U+00C4 (A with diaeresis), U+00C9/U+00E9 (E with acute)
        const QString upperU(QChar(0x00DC)), lowerU(QChar(0x00FC));
        const QString upperA(QChar(0x00C4)), upperE(QChar(0x00C9)), lowerE(QChar(0x00E9));

        QVERIFY(repo.addTask(draft(upperU + "ber uns")));
        QVERIFY(repo.addTask(draft(upperA + "pfel kaufen")));
        QVERIFY(repo.addTask(draft(upperE + "COLE")));
        QVERIFY(repo.addTask(draft("Plain ASCII")));

        auto count = [&](const QString &needle) {
            TaskRepository::Query query;
            query.titleContains = needle;
            QList<Task> found;
            return repo.loadTasks(query, &found) ? found.size() : -1;
        };
        QCOMPARE(count(lowerU + "ber"), 1);              // lower-case needle finds upper-case title
        QCOMPARE(count(upperA + "PFEL"), 1);
        QCOMPARE(count(lowerE + "cole"), 1);
        QCOMPARE(count("ascii"), 1);
        QCOMPARE(count("PLAIN"), 1);
        QCOMPARE(count("  plain  "), 1);                 // surrounding spaces ignored
        QCOMPARE(count("nothing"), 0);
        QCOMPARE(count(QString()), 4);                   // empty = everything
    }

    void model_defaultOrderIsOpenFirstThenDueThenPriority()
    {
        Database db;
        QVERIFY(db.initialize(path("order.db")));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.refresh());

        const QDateTime soon(QDate(2031, 1, 1), QTime(9, 0), QTimeZone::utc());
        const QDateTime later(QDate(2031, 6, 1), QTime(9, 0), QTimeZone::utc());
        QVERIFY(model.addTask("no date, low", "", "", 0));
        QVERIFY(model.addTask("later", "", "", 0, later));
        QVERIFY(model.addTask("soon", "", "", 0, soon));
        QVERIFY(model.addTask("no date, high", "", "", 2));
        QVERIFY(model.addTask("soon, high", "", "", 2, soon));

        auto titles = [&]() {
            QStringList result;
            for (int row = 0; row < model.rowCount(); ++row)
                result << model.data(model.index(row), TaskListModel::TitleRole).toString();
            return result;
        };
        // due date ascending (undated last); same due date: higher priority first;
        // otherwise creation order
        QCOMPARE(titles(), (QStringList{"soon, high", "soon", "later", "no date, high", "no date, low"}));

        // the same order comes back from the database after a reload
        QVERIFY(model.refresh());
        QCOMPARE(titles(), (QStringList{"soon, high", "soon", "later", "no date, high", "no date, low"}));

        // completing moves a task below the open ones, with a move (not a rebuild)
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        const qint64 soonHighId = model.data(model.index(0), TaskListModel::IdRole).toLongLong();
        QVERIFY(model.setCompleted(soonHighId, true));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(titles(), (QStringList{"soon", "later", "no date, high", "no date, low", "soon, high"}));

        // reopening puts it back where its due date and priority say
        QVERIFY(model.setCompleted(soonHighId, false));
        QCOMPARE(titles(), (QStringList{"soon, high", "soon", "later", "no date, high", "no date, low"}));

        // editing the due date re-sorts
        const qint64 laterId = model.data(model.index(2), TaskListModel::IdRole).toLongLong();
        QVERIFY(model.updateTask(laterId, "later", "", "Inbox", 0, soon.addDays(-10)));
        QCOMPARE(titles().first(), QString("later"));

        // and a reload agrees with the incremental result
        const QStringList incremental = titles();
        QVERIFY(model.refresh());
        QCOMPARE(titles(), incremental);
    }

    // ---- views: due date ranges and lists ---------------------------------

    void repo_filtersByDueRangeAndList()
    {
        Database db;
        QVERIFY(db.initialize(path("range.db")));
        TaskRepository repo(db);

        Task early = draft("early");
        early.dueAt = QDateTime(QDate(2030, 1, 1), QTime(10, 0), QTimeZone::utc());
        Task late = draft("late");
        late.dueAt = QDateTime(QDate(2030, 1, 3), QTime(10, 0), QTimeZone::utc());
        Task undated = draft("undated");
        Task work = draft("work item");
        work.listName = "Work";
        QVERIFY(repo.addTask(early) && repo.addTask(late) && repo.addTask(undated) && repo.addTask(work));

        auto titles = [&](TaskRepository::Query query) {
            QList<Task> found;
            if (!repo.loadTasks(query, &found))
                return QStringList{"<error>"};
            QStringList result;
            for (const Task &task : found)
                result << task.title;
            return result;
        };

        TaskRepository::Query range;
        range.dueFrom = QDateTime(QDate(2030, 1, 1), QTime(0, 0), QTimeZone::utc());
        range.dueBefore = QDateTime(QDate(2030, 1, 2), QTime(0, 0), QTimeZone::utc());
        QCOMPARE(titles(range), QStringList{"early"}); // undated never matches

        TaskRepository::Query from;
        from.dueFrom = QDateTime(QDate(2030, 1, 2), QTime(0, 0), QTimeZone::utc());
        QCOMPARE(titles(from), QStringList{"late"});

        TaskRepository::Query inList;
        inList.listName = "Work";
        QCOMPARE(titles(inList), QStringList{"work item"});
        inList.titleContains = "nothing";
        QCOMPARE(titles(inList), QStringList()); // composes with search

        QStringList names;
        QVERIFY(repo.listNames(&names));
        QCOMPARE(names, (QStringList{"Inbox", "Work"}));
    }

    void model_scopesComposeWithFiltersAndSearch()
    {
        Database db;
        QVERIFY(db.initialize(path("scopes.db")));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.refresh());

        const QDate today = QDate::currentDate();
        const QDateTime todayLate(today, QTime(23, 59));
        const QDateTime tomorrow(today.addDays(1), QTime(12, 0));
        const QDateTime yesterday(today.addDays(-1), QTime(12, 0));

        QVERIFY(model.addTask("today open", "", "Inbox", 0, todayLate));
        QVERIFY(model.addTask("today done", "", "Inbox", 0, todayLate));
        QVERIFY(model.addTask("tomorrow", "", "Work", 0, tomorrow));
        QVERIFY(model.addTask("yesterday", "", "Inbox", 0, yesterday));
        QVERIFY(model.addTask("no date", "", "Work"));
        auto idOf = [&](const QString &title) {
            for (int row = 0; row < model.rowCount(); ++row) {
                if (model.data(model.index(row), TaskListModel::TitleRole).toString() == title)
                    return model.data(model.index(row), TaskListModel::IdRole).toLongLong();
            }
            return qint64(-1);
        };
        const qint64 doneId = idOf("today done");
        QVERIFY(model.setCompleted(doneId, true));
        const qint64 tomorrowId = idOf("tomorrow");

        auto titles = [&]() {
            QStringList result;
            for (int row = 0; row < model.rowCount(); ++row)
                result << model.data(model.index(row), TaskListModel::TitleRole).toString();
            return result;
        };

        QVERIFY(model.setView(TaskListModel::Today));
        QCOMPARE(titles(), (QStringList{"today open", "today done"})); // includes completed
        model.setCompletionFilter(TaskListModel::Active);
        QCOMPARE(titles(), QStringList{"today open"});
        model.setCompletionFilter(TaskListModel::All);

        QVERIFY(model.setView(TaskListModel::Upcoming));
        QCOMPARE(titles(), QStringList{"tomorrow"}); // incomplete, after today
        model.setSearchText("zzz");
        QCOMPARE(model.rowCount(), 0);
        model.setSearchText(QString());

        // A task completed while viewing Upcoming leaves the view.
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QVERIFY(model.setCompleted(tomorrowId, true));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.setCompleted(tomorrowId, false));
        QCOMPARE(model.rowCount(), 1); // and returns when re-opened

        QVERIFY(model.setView(TaskListModel::List, "Work"));
        QCOMPARE(titles(), (QStringList{"tomorrow", "no date"}));
        QCOMPARE(model.listName(), QString("Work"));
        QVERIFY(model.setView(TaskListModel::Everything, QString(), TaskListModel::Completed));
        QCOMPARE(titles(), QStringList{"today done"});

        QCOMPARE(model.listNames(), (QStringList{"Inbox", "Work"}));
        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QVERIFY(!model.setView(TaskListModel::List, "  "));
        QVERIFY(!model.setView(99));
        QCOMPARE(errors.size(), 2);
    }

    // ---- completed-task cleanup -------------------------------------------

private:
    // Rewrites a stored completion time (through a separate connection), to
    // simulate a task that was completed `minutesAgo` minutes ago.
    void completedMinutesAgo(const QString &file, qint64 id, qint64 minutesAgo)
    {
        const QString when = QDateTime::currentDateTimeUtc()
                                 .addSecs(-minutesAgo * 60)
                                 .toString(Qt::ISODateWithMs);
        QVERIFY(runSql(file, {QStringLiteral("UPDATE tasks SET completed_at = '%1' WHERE id = %2")
                                  .arg(when)
                                  .arg(id)}));
    }

    static constexpr qint64 kDay = 24 * 60;

    // Adds a task through the model; returns its id (0 on failure).
    static qint64 addVia(TaskListModel &model, const QString &title)
    {
        if (!model.addTask(title))
            return 0;
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row), TaskListModel::TitleRole).toString() == title)
                return model.data(model.index(row), TaskListModel::IdRole).toLongLong();
        }
        return 0;
    }

    static QStringList storedTitles(const QString &file)
    {
        QStringList titles;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kSeedConnection);
            db.setDatabaseName(file);
            if (db.open()) {
                QSqlQuery query(db);
                if (query.exec(QStringLiteral("SELECT title FROM tasks ORDER BY id"))) {
                    while (query.next())
                        titles << query.value(0).toString();
                }
            }
        }
        QSqlDatabase::removeDatabase(kSeedConnection);
        return titles;
    }

private slots:
    void cleanup_completionTimeIsStoredAndCleared()
    {
        Database db;
        QVERIFY(db.initialize(path("cleanup_stamp.db")));
        TaskRepository repo(db);
        Task created;
        QVERIFY(repo.addTask(draft("a"), &created));
        QVERIFY(!created.completedAt.has_value());

        Task done;
        QVERIFY(repo.setCompleted(created.id, true, &done));
        QVERIFY(done.completedAt.has_value());
        const QDateTime first = *done.completedAt;

        // Completing again does not extend the retention clock.
        QTest::qWait(5);
        Task again;
        QVERIFY(repo.setCompleted(created.id, true, &again));
        QCOMPARE(*again.completedAt, first);

        Task reopened;
        QVERIFY(repo.setCompleted(created.id, false, &reopened));
        QVERIFY(!reopened.completedAt.has_value());
    }

    void cleanup_upgradeBackfillsCompletionTimeAndDeletesNothing()
    {
        const QString file = path("cleanup_v2.db");
        QStringList seed = v1Statements();
        seed << "CREATE TABLE tasks (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                "title TEXT NOT NULL CHECK (length(trim(title)) > 0), notes TEXT NOT NULL DEFAULT '', "
                "list_name TEXT NOT NULL DEFAULT 'Inbox', "
                "priority INTEGER NOT NULL DEFAULT 0 CHECK (priority BETWEEN 0 AND 2), due_at TEXT, "
                "completed INTEGER NOT NULL DEFAULT 0 CHECK (completed IN (0, 1)), reminder_at TEXT, "
                "reminder_sent INTEGER NOT NULL DEFAULT 0 CHECK (reminder_sent IN (0, 1)), "
                "created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"
             << "INSERT INTO tasks (title, completed, created_at, updated_at) VALUES "
                "('old done', 1, '2001-01-01T00:00:00.000Z', '2001-02-03T04:05:06.000Z')"
             << "INSERT INTO tasks (title, completed, created_at, updated_at) VALUES "
                "('open', 0, '2001-01-01T00:00:00.000Z', '2001-01-01T00:00:00.000Z')"
             << "UPDATE app_metadata SET value = '2' WHERE key = 'schema_version'"
             << "PRAGMA user_version = 2";
        QVERIFY(runSql(file, seed));
        const QByteArray original = fileHash(file);

        {
            Database db;
            QVERIFY2(db.initialize(file), qPrintable(db.lastError()));
            TaskRepository repo(db);
            TaskListModel model(repo);
            QVERIFY(model.loadCleanupPolicy());
            QCOMPARE(model.cleanupMode(), static_cast<int>(CleanupPolicy::Mode::Never));
            QCOMPARE(model.applyCleanup(), 0); // an upgrade never deletes anything
        }
        QCOMPARE(storedTitles(file), (QStringList{"old done", "open"}));
        QCOMPARE(scalar(file, "SELECT completed_at FROM tasks WHERE title='old done'").toString(),
                 QString("2001-02-03T04:05:06.000Z"));
        QVERIFY(scalar(file, "SELECT completed_at FROM tasks WHERE title='open'").isNull());
        QCOMPARE(fileHash(file + ".v2.bak"), original);
    }

    void cleanup_defaultPolicyNeverDeletesCompletedTasks()
    {
        const QString file = path("cleanup_default.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.loadCleanupPolicy());
        QCOMPARE(model.cleanupMode(), 0);

        const qint64 id = addVia(model, "ancient");
        QVERIFY(model.setCompleted(id, true));
        completedMinutesAgo(file, id, 5000 * kDay);

        QCOMPARE(model.applyCleanup(), 0);
        QVERIFY(model.setCompleted(addVia(model, "another"), true)); // a completion event
        QCOMPARE(storedTitles(file), (QStringList{"ancient", "another"}));
        QCOMPARE(model.rowCount(), 2);
    }

    void cleanup_immediateDeletesOnCompletionOnly()
    {
        const QString file = path("cleanup_immediate.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.loadCleanupPolicy());
        const qint64 keep = addVia(model, "stays active");
        const qint64 done = addVia(model, "finish me");
        QVERIFY(keep > 0 && done > 0);

        QCOMPARE(model.previewCleanup(1, 0), 0);
        QVERIFY(model.setCleanupPolicy(1, 0));
        QCOMPARE(model.cleanupMode(), 1);
        QCOMPARE(model.rowCount(), 2); // saving the policy touched no active task

        QSignalSpy removedRows(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy dataChanged(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy notified(&model, &TaskListModel::completedTasksRemoved);
        QVERIFY2(model.setCompleted(done, true), qPrintable(model.lastError()));

        QCOMPARE(removedRows.size(), 1);
        QCOMPARE(dataChanged.size(), 0); // never shown as a completed row
        QCOMPARE(notified.size(), 1);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(storedTitles(file), QStringList{"stays active"});
        QVERIFY(!model.canUndo());

        // Reopening and editing never resurrect it.
        QVERIFY(!model.setCompleted(done, false));
        QVERIFY(!model.updateTask(done, "ghost", "", "Inbox", 0));
        QCOMPARE(storedTitles(file), QStringList{"stays active"});
    }

    void cleanup_afterDaysUsesCutoffAndSparesFreshAndActiveTasks()
    {
        const QString file = path("cleanup_days.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);

        const qint64 old = addVia(model, "old");
        const qint64 justOver = addVia(model, "just over");
        const qint64 justUnder = addVia(model, "just under");
        const qint64 fresh = addVia(model, "fresh");
        const qint64 unknown = addVia(model, "unknown age");
        const qint64 active = addVia(model, "active but ancient");
        for (qint64 id : {old, justOver, justUnder, fresh, unknown})
            QVERIFY(model.setCompleted(id, true));
        completedMinutesAgo(file, old, 30 * kDay);
        completedMinutesAgo(file, justOver, 7 * kDay + 1);
        completedMinutesAgo(file, justUnder, 7 * kDay - 1);
        QVERIFY(runSql(file, {QStringLiteral("UPDATE tasks SET completed_at = NULL WHERE id = %1").arg(unknown),
                              QStringLiteral("UPDATE tasks SET created_at = '2001-01-01T00:00:00.000Z' "
                                             "WHERE id = %1").arg(active)}));

        QCOMPARE(model.previewCleanup(2, 7), 2);
        QCOMPARE(storedTitles(file).size(), 6); // previewing deletes nothing
        QCOMPARE(model.previewCleanup(2, 0), -1);   // invalid retention
        QCOMPARE(model.previewCleanup(2, 99999), -1);

        QSignalSpy notified(&model, &TaskListModel::completedTasksRemoved);
        QVERIFY(model.setCleanupPolicy(2, 7));
        QCOMPARE(model.cleanupDays(), 7);
        QCOMPARE(storedTitles(file),
                 (QStringList{"just under", "fresh", "unknown age", "active but ancient"}));
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(notified.value(0).value(0).toInt(), 2);
        QVERIFY(!model.canUndo());
    }

    void cleanup_afterDaysRunsAtCompletionAndStartup()
    {
        const QString file = path("cleanup_events.db");
        qint64 stale = 0;
        {
            Database db;
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            TaskListModel model(repo);
            QVERIFY(model.setCleanupPolicy(2, 1));
            stale = addVia(model, "stale");
            QVERIFY(model.setCompleted(stale, true));
            completedMinutesAgo(file, stale, 3 * kDay);
            QCOMPARE(storedTitles(file), QStringList{"stale"}); // not yet: no event since

            const qint64 next = addVia(model, "next");
            QVERIFY(model.setCompleted(next, true)); // completion event
            QCOMPARE(storedTitles(file), QStringList{"next"});
            completedMinutesAgo(file, next, 3 * kDay);
        }
        // "Application startup": a new model loads the saved policy and applies it.
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        QVERIFY(model.loadCleanupPolicy());
        QCOMPARE(model.cleanupDays(), 1);
        QCOMPARE(model.applyCleanup(), 1);
        QVERIFY(model.refresh());
        QCOMPARE(model.rowCount(), 0);
    }

    void cleanup_manualClearRemovesOnlyCompletedAndIsPermanent()
    {
        const QString file = path("cleanup_manual.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);

        const qint64 a = addVia(model, "done 1");
        const qint64 b = addVia(model, "done 2");
        addVia(model, "open 1");
        addVia(model, "open 2");
        QVERIFY(model.setCompleted(a, true));
        QVERIFY(model.setCompleted(b, true));
        QCOMPARE(model.completedCount(), 2);

        QCOMPARE(model.clearCompleted(), 2); // policy is "never"; manual always works
        QCOMPARE(storedTitles(file), (QStringList{"open 1", "open 2"}));
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.completedCount(), 0);
        QCOMPARE(model.clearCompleted(), 0);
        QVERIFY(!model.canUndo());
        QVERIFY(!model.undoDelete());
    }

    void cleanup_invalidOrMissingSettingFallsBackToNever()
    {
        const QString file = path("cleanup_invalid.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);

        QVERIFY(model.loadCleanupPolicy()); // nothing stored yet
        QCOMPARE(model.cleanupMode(), 0);

        const QStringList bad = {"garbage", "", "days:0", "days:-3", "days:abc", "days:99999",
                                 "DAYS:7", "immediate"};
        for (const QString &value : bad) {
            QVERIFY(runSql(file, {QStringLiteral("INSERT INTO app_metadata (key, value) VALUES "
                                                 "('completed_cleanup_policy', '%1') "
                                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value")
                                      .arg(value)}));
            QVERIFY(model.loadCleanupPolicy());
            QCOMPARE(model.cleanupMode(), 0);
            QCOMPARE(model.applyCleanup(), 0);
            // the stored value is left alone, not "repaired"
            QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='completed_cleanup_policy'")
                         .toString(), value);
        }
        QVERIFY(!model.setCleanupPolicy(7, 0)); // unknown mode refused
        QVERIFY(!model.setCleanupPolicy(2, 0));
        QCOMPARE(model.cleanupMode(), 0);
    }

    void cleanup_policyPersistsAcrossRestart()
    {
        const QString file = path("cleanup_persist.db");
        {
            Database db;
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            TaskListModel model(repo);
            QVERIFY(model.setCleanupPolicy(2, 14));
        }
        {
            Database db;
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            TaskListModel model(repo);
            QCOMPARE(model.cleanupMode(), 0); // nothing is active until it is loaded
            QVERIFY(model.loadCleanupPolicy());
            QCOMPARE(model.cleanupMode(), 2);
            QCOMPARE(model.cleanupDays(), 14);
            QVERIFY(model.setCleanupPolicy(0, 0));
        }
        {
            Database db;
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            TaskListModel model(repo);
            QVERIFY(model.loadCleanupPolicy());
            QCOMPARE(model.cleanupMode(), 0);
        }
        // other metadata survived
        QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='schema_version'").toString(),
                 QString("3"));
    }

    void cleanup_removedTaskCannotBeUndoneOrResurrected()
    {
        const QString file = path("cleanup_undo.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);

        // An ordinary delete is undoable...
        const qint64 manual = addVia(model, "deleted by hand");
        QVERIFY(model.removeTask(manual));
        QVERIFY(model.canUndo());

        // ...a policy deletion is not, and does not disturb that snapshot.
        QVERIFY(model.setCleanupPolicy(1, 0));
        const qint64 auto1 = addVia(model, "removed by policy");
        QVERIFY(model.setCompleted(auto1, true));
        QVERIFY(model.canUndo());
        QCOMPARE(model.undoTitle(), QString("deleted by hand"));
        QVERIFY(model.undoDelete());
        QCOMPARE(storedTitles(file), QStringList{"deleted by hand"});
        QVERIFY(!model.canUndo());
        QVERIFY(!model.undoDelete()); // nothing left that could bring auto1 back

        // Same for the manual "clear completed" action.
        QVERIFY(model.setCleanupPolicy(0, 0));
        const qint64 manual2 = addVia(model, "cleared");
        QVERIFY(model.setCompleted(manual2, true));
        QCOMPARE(model.clearCompleted(), 1);
        QVERIFY(!model.canUndo());
        QVERIFY(!model.undoDelete());
        QCOMPARE(storedTitles(file), QStringList{"deleted by hand"});

        // A restored completed task keeps its completion time (so retention
        // still counts from when it was really completed).
        const qint64 oldDone = addVia(model, "old done");
        QVERIFY(model.setCompleted(oldDone, true));
        completedMinutesAgo(file, oldDone, 40 * kDay);
        QVERIFY(model.refresh());
        QVERIFY(model.removeTask(oldDone));
        QVERIFY(model.undoDelete());
        QCOMPARE(model.previewCleanup(2, 30), 1);
    }

    void cleanup_reminderTimerAndInFlightStateAreReleased()
    {
        SchedulerFixture f(path("cleanup_reminder.db"), kStart);
        QVERIFY(f.ok);
        TaskListModel model(f.repo);
        QObject::connect(&model, &TaskListModel::tasksMutated, &f.scheduler,
                         &ReminderScheduler::reschedule);
        QVERIFY(model.refresh());
        QVERIFY(model.setCleanupPolicy(1, 0));

        // A pending reminder arms the single timer...
        const qint64 pending = f.addReminder("pending", 30);
        QVERIFY(model.refresh());
        f.scheduler.reschedule();
        QVERIFY(f.scheduler.isTimerActive());

        // ...and completing the task under "immediately" releases it.
        QVERIFY(model.setCompleted(pending, true));
        QVERIFY(!f.scheduler.isTimerActive());
        QVERIFY(f.scheduler.nextReminderAt().isNull());

        // An announced (in-flight) reminder is cancelled, so the banner goes.
        const qint64 fired = f.addReminder("fired", 5);
        QVERIFY(model.refresh());
        f.now = kStart.addSecs(10 * 60);
        f.scheduler.processDue();
        QCOMPARE(f.scheduler.inFlightIds(), QList<qint64>{fired});
        f.cancelled.clear();
        QVERIFY(model.setCompleted(fired, true));
        QCOMPARE(f.cancelled, QList<qint64>{fired});
        QVERIFY(f.scheduler.inFlightIds().isEmpty());
        QVERIFY(!f.scheduler.isTimerActive());
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(scalar(path("cleanup_reminder.db"), "SELECT COUNT(*) FROM tasks").toInt(), 0);

        // A reminder cannot bring a removed task back: acknowledging it is harmless.
        QVERIFY(f.scheduler.acknowledge(fired));
        QCOMPARE(scalar(path("cleanup_reminder.db"), "SELECT COUNT(*) FROM tasks").toInt(), 0);
    }

    void cleanup_batchPurgeCancelsRemovedTasksReminders()
    {
        // Completed tasks do not hold timers (reminders are for open tasks), but
        // a task that was announced and then completed elsewhere is in flight
        // until the scheduler revalidates; a purge must leave nothing behind.
        SchedulerFixture f(path("cleanup_batch.db"), kStart);
        QVERIFY(f.ok);
        TaskListModel model(f.repo);
        QObject::connect(&model, &TaskListModel::tasksMutated, &f.scheduler,
                         &ReminderScheduler::reschedule);
        const qint64 id = f.addReminder("rem", 5);
        QVERIFY(model.refresh());
        f.now = kStart.addSecs(10 * 60);
        f.scheduler.processDue();
        QCOMPARE(f.scheduler.inFlightIds(), QList<qint64>{id});
        QVERIFY(f.repo.setCompleted(id, true)); // completed behind the model's back
        QCOMPARE(model.clearCompleted(), 1);
        QVERIFY(f.scheduler.inFlightIds().isEmpty());
        QVERIFY(!f.scheduler.isTimerActive());
        QCOMPARE(model.rowCount(), 0); // the stale cached row is gone too
    }

    void cleanup_databaseFailuresAreReportedAndChangeNothing()
    {
        const QString file = path("cleanup_fail.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        const qint64 a = addVia(model, "done");
        addVia(model, "open");
        QVERIFY(model.setCompleted(a, true));

        // A trigger makes every DELETE fail inside the real database.
        QVERIFY(runSql(file, {QStringLiteral("CREATE TRIGGER block_delete BEFORE DELETE ON tasks "
                                             "BEGIN SELECT RAISE(ABORT, 'delete blocked'); END")}));
        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QSignalSpy notified(&model, &TaskListModel::completedTasksRemoved);

        QCOMPARE(model.clearCompleted(), -1);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("blocked"));
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(storedTitles(file).size(), 2);

        // Saving a policy that would delete: saved, but the failure is reported
        // and nothing is claimed as removed.
        QVERIFY(!model.setCleanupPolicy(1, 0));
        QCOMPARE(notified.size(), 0);
        QCOMPARE(model.rowCount(), 2);

        // "Immediately": the completion fails as a whole; the task stays active.
        const qint64 b = addVia(model, "open 2");
        errors.clear();
        QVERIFY(!model.setCompleted(b, true));
        QVERIFY(errors.size() >= 1);
        QCOMPARE(scalar(file, QStringLiteral("SELECT completed FROM tasks WHERE id=%1").arg(b)).toInt(), 0);
        QCOMPARE(notified.size(), 0);
        QCOMPARE(model.rowCount(), 3);

        // A closed database is reported, not ignored.
        QVERIFY(runSql(file, {QStringLiteral("DROP TRIGGER block_delete")}));
        db.shutdown();
        errors.clear();
        QCOMPARE(model.applyCleanup(), -1);
        QCOMPARE(model.clearCompleted(), -1);
        QCOMPARE(model.completedCount(), -1);
        QVERIFY(!model.setCleanupPolicy(0, 0));
        QVERIFY(errors.size() >= 4);
        QCOMPARE(model.cleanupMode(), 1); // unchanged: the save did not happen
    }

    void cleanup_searchAndViewsNoLongerShowRemovedTasks()
    {
        const QString file = path("cleanup_views.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        const qint64 a = addVia(model, "needle done");
        addVia(model, "needle open");
        QVERIFY(model.setCompleted(a, true));

        model.setSearchText("needle");
        QCOMPARE(model.rowCount(), 2);
        QVERIFY(model.setCleanupPolicy(1, 0)); // existing completed task removed on save
        QCOMPARE(model.rowCount(), 1);
        QVERIFY(model.setView(TaskListModel::Everything, QString(), TaskListModel::Completed));
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.refresh());
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(model.setView(TaskListModel::Everything, QString(), TaskListModel::All));
        QCOMPARE(model.rowCount(), 1);
    }

    // ---- date/time values: UTC storage and reminder scheduling ---------------

    void datetime_localTimeIsStoredAsUtcWithSeconds()
    {
        const QString file = path("dt_utc.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);

        const QDateTime due(QDate(2031, 7, 1), QTime(14, 30, 15), Qt::LocalTime);
        const QDateTime reminder(QDate(2031, 7, 1), QTime(0, 0, 0), Qt::LocalTime); // midnight
        QVERIFY(model.addTask("dt", "", "", 0, due, reminder));

        QCOMPARE(scalar(file, "SELECT due_at FROM tasks").toString(),
                 due.toUTC().toString(Qt::ISODateWithMs));
        QCOMPARE(scalar(file, "SELECT reminder_at FROM tasks").toString(),
                 reminder.toUTC().toString(Qt::ISODateWithMs));
        // read back as the same instants, in local time, seconds kept
        const QModelIndex row = model.index(0);
        QCOMPARE(model.data(row, TaskListModel::DueAtRole).toDateTime(), due);
        QCOMPARE(model.data(row, TaskListModel::DueAtRole).toDateTime().time().second(), 15);
        QCOMPARE(model.data(row, TaskListModel::ReminderAtRole).toDateTime(), reminder);

        // a leap day survives the round trip
        const QDateTime leap(QDate(2032, 2, 29), QTime(12, 0), Qt::LocalTime);
        const qint64 id = addVia(model, "dt");
        QVERIFY(model.updateTaskWithReminder(id, "dt", "", "Inbox", 0, leap, QDateTime()));
        QCOMPARE(model.data(model.index(0), TaskListModel::DueAtRole).toDateTime(), leap);
        QCOMPARE(model.data(model.index(0), TaskListModel::DueAtRole).toDateTime().date(),
                 QDate(2032, 2, 29));
    }

    void datetime_editingTheReminderRearmsTheScheduler()
    {
        SchedulerFixture f(path("dt_resched.db"), kStart);
        QVERIFY(f.ok);
        TaskListModel model(f.repo);
        QObject::connect(&model, &TaskListModel::tasksMutated, &f.scheduler,
                         &ReminderScheduler::reschedule);

        QVERIFY(model.addTask("r", "", "", 0, QDateTime(), kStart.addSecs(3600)));
        const qint64 id = model.data(model.index(0), TaskListModel::IdRole).toLongLong();
        QCOMPARE(f.scheduler.nextReminderAt(), kStart.addSecs(3600));
        QCOMPARE(f.scheduler.timerInterval(), 3600 * 1000);

        // moved earlier, to a time with seconds: the single timer is re-armed
        const QDateTime sooner = kStart.addSecs(90);
        QVERIFY(model.updateTaskWithReminder(id, "r", "", "Inbox", 0, QDateTime(), sooner));
        QCOMPARE(f.scheduler.nextReminderAt(), sooner);
        QCOMPARE(f.scheduler.timerInterval(), 90 * 1000);
        QVERIFY(f.scheduler.isTimerActive());

        // it fires once, is acknowledged, and a new time re-arms it
        f.now = kStart.addSecs(120);
        f.scheduler.processDue();
        QCOMPARE(f.fired, QList<qint64>{id});
        QVERIFY(f.scheduler.acknowledge(id));
        QVERIFY(!f.scheduler.isTimerActive());

        const QDateTime later = kStart.addSecs(2 * 3600);
        QVERIFY(model.updateTaskWithReminder(id, "r", "", "Inbox", 0, QDateTime(), later));
        QCOMPARE(f.scheduler.nextReminderAt(), later);
        QCOMPARE(f.fired.size(), 1);

        // clearing the reminder leaves nothing armed
        QVERIFY(model.updateTaskWithReminder(id, "r", "", "Inbox", 0, QDateTime(), QDateTime()));
        QVERIFY(!f.scheduler.isTimerActive());
    }

    // ---- floating widget ---------------------------------------------------------

    void widget_defaultsAreSensibleAndNothingIsStoredYet()
    {
        const QString file = path("widget_defaults.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        WidgetController widget(model, repo);
        QVERIFY(widget.load());

        QVERIFY(!widget.widgetVisible());      // hidden until the user shows it
        QVERIFY(!widget.hasPosition());
        QCOMPARE(widget.width(), WidgetController::kDefaultWidth);
        QCOMPARE(widget.height(), WidgetController::kDefaultHeight);
        QVERIFY(widget.showToday() && widget.showUpcoming() && widget.showQuickAdd()
                && widget.showReminders() && widget.showDetails());
        QVERIFY(widget.alwaysOnTop());
        QVERIFY(!widget.compact());
        QVERIFY(widget.opacity() >= WidgetController::kMinOpacity && widget.opacity() <= 1.0);
        QVERIFY(scalar(file, "SELECT value FROM app_metadata WHERE key='widget_settings'").isNull());
    }

    void widget_preferencesPersistAcrossRestart()
    {
        const QString file = path("widget_persist.db");
        {
            Database db;
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            TaskListModel model(repo);
            WidgetController widget(model, repo);
            QVERIFY(widget.load());
            widget.setWidgetVisible(true);
            widget.setShowOnLaunch(true);
            widget.setX(-120);
            widget.setY(340);
            widget.setWidth(401);
            widget.setHeight(333);
            widget.setCompact(true);
            widget.setSurface(2);
            widget.setOpacity(0.7);
            widget.setAlwaysOnTop(false);
            widget.setShowUpcoming(false);
            widget.setShowQuickAdd(false);
            widget.setShowDetails(false);
            // written shortly after the last change, without an explicit flush
            QTRY_VERIFY(!scalar(file, "SELECT value FROM app_metadata WHERE key='widget_settings'").isNull());
            QTest::qWait(700);
        }
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        WidgetController widget(model, repo);
        QVERIFY(widget.load());
        QVERIFY(widget.showOnLaunch());
        QVERIFY(widget.widgetVisible()); // shown because "show on launch" is on
        QVERIFY(widget.hasPosition());
        QCOMPARE(widget.x(), -120);
        QCOMPARE(widget.y(), 340);
        QCOMPARE(widget.width(), 401);
        QCOMPARE(widget.height(), 333);
        QVERIFY(widget.compact());
        QCOMPARE(widget.surface(), 2);
        QCOMPARE(widget.opacity(), 0.7);
        QVERIFY(!widget.alwaysOnTop());
        QVERIFY(widget.showToday());
        QVERIFY(!widget.showUpcoming());
        QVERIFY(!widget.showQuickAdd());
        QVERIFY(widget.showReminders());
        QVERIFY(!widget.showDetails());
    }

    void widget_pendingChangesAreWrittenWhenTheControllerIsDestroyed()
    {
        const QString file = path("widget_destroy.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        {
            WidgetController widget(model, repo);
            widget.setWidth(512); // save timer pending
        }
        QVERIFY(scalar(file, "SELECT value FROM app_metadata WHERE key='widget_settings'")
                    .toString().contains("512"));
        // the model keeps working with the controller gone
        QVERIFY(model.addTask("still works"));
    }

    void widget_invalidStoredPreferencesFallBackToSafeValues()
    {
        const QString file = path("widget_invalid.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);

        auto store = [&](const QString &json) {
            QVERIFY(runSql(file, {QStringLiteral("INSERT INTO app_metadata (key, value) VALUES "
                                                 "('widget_settings', '%1') ON CONFLICT(key) "
                                                 "DO UPDATE SET value = excluded.value").arg(json)}));
        };

        store("not json at all");
        {
            WidgetController widget(model, repo);
            QVERIFY(widget.load());
            QVERIFY(!widget.widgetVisible());
            QCOMPARE(widget.width(), WidgetController::kDefaultWidth);
        }
        store("[1,2,3]");
        {
            WidgetController widget(model, repo);
            QVERIFY(widget.load());
            QVERIFY(widget.showToday());
        }
        // out-of-range values are clamped, wrong types ignored
        store("{\"width\":5,\"height\":99999,\"opacity\":0.05,\"surface\":9,"
              "\"showToday\":\"yes\",\"x\":\"left\",\"visible\":true}");
        {
            WidgetController widget(model, repo);
            QVERIFY(widget.load());
            QCOMPARE(widget.width(), WidgetController::kMinWidth);
            QVERIFY(widget.height() <= 4000);
            QCOMPARE(widget.opacity(), WidgetController::kMinOpacity);
            QCOMPARE(widget.surface(), WidgetController::kSurfaceCount - 1);
            QVERIFY(widget.showToday());     // not a bool: default kept
            QVERIFY(!widget.hasPosition());  // x/y need to be numbers
            // A saved "visible":true from an earlier session is not restored.
            QVERIFY(!widget.showOnLaunch());
            QVERIFY(!widget.widgetVisible());
        }
        store("{\"visible\":false,\"showOnLaunch\":true}");
        {
            WidgetController widget(model, repo);
            QVERIFY(widget.load());
            QVERIFY(widget.widgetVisible());
        }
        store("{\"showOnLaunch\":\"yes\"}"); // wrong type: stays off
        {
            WidgetController widget(model, repo);
            QVERIFY(widget.load());
            QVERIFY(!widget.widgetVisible());
        }
        // setters clamp too
        WidgetController widget(model, repo);
        widget.setOpacity(5.0);
        QCOMPARE(widget.opacity(), 1.0);
        widget.setOpacity(-1.0);
        QCOMPARE(widget.opacity(), WidgetController::kMinOpacity);
        widget.setWidth(1);
        QCOMPARE(widget.width(), WidgetController::kMinWidth);
    }

    void widget_positionIsRecoveredWhenOffScreen()
    {
        const QRect primary(0, 0, 1920, 1040);
        const QRect second(1920, 0, 1600, 900);
        QPoint p;

        // on screen: unchanged
        QVERIFY(WidgetController::recoverPosition(QRect(100, 80, 320, 460), {primary}, &p));
        QCOMPARE(p, QPoint(100, 80));
        // on a second screen that still exists: unchanged
        QVERIFY(WidgetController::recoverPosition(QRect(2000, 50, 320, 460), {primary, second}, &p));
        QCOMPARE(p, QPoint(2000, 50));
        // the second screen was unplugged: pulled onto the remaining one
        QVERIFY(WidgetController::recoverPosition(QRect(2000, 50, 320, 460), {primary}, &p));
        QCOMPARE(p, QPoint(1920 - 320, 50));
        // far away / negative coordinates
        QVERIFY(WidgetController::recoverPosition(QRect(-5000, -3000, 320, 460), {primary}, &p));
        QCOMPARE(p, QPoint(0, 0));
        QVERIFY(WidgetController::recoverPosition(QRect(40000, 40000, 320, 460), {primary}, &p));
        QCOMPARE(p, QPoint(1920 - 320, 1040 - 460));
        // mostly off the left edge but the title strip is still reachable: kept
        QVERIFY(WidgetController::recoverPosition(QRect(-230, 100, 320, 460), {primary}, &p));
        QCOMPARE(p, QPoint(-230, 100));
        // only a sliver visible: moved back
        QVERIFY(WidgetController::recoverPosition(QRect(-310, 100, 320, 460), {primary}, &p));
        QCOMPARE(p, QPoint(0, 100));
        // title strip above the top of the screen cannot be grabbed: moved down
        QVERIFY(WidgetController::recoverPosition(QRect(100, -200, 320, 460), {primary}, &p));
        QCOMPARE(p, QPoint(100, 0));
        // a window taller than the screen is aligned to the top-left
        QVERIFY(WidgetController::recoverPosition(QRect(-9000, 500, 320, 3000), {primary}, &p));
        QCOMPARE(p, QPoint(0, 0));
        // no screens: nothing can be said
        QVERIFY(!WidgetController::recoverPosition(QRect(0, 0, 320, 460), {}, &p));
    }

    void widget_viewsFollowTheMainModelWithoutPolling()
    {
        const QString file = path("widget_views.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        WidgetController widget(model, repo);
        widget.refreshViews();
        auto *today = qobject_cast<TaskListModel *>(widget.todayModel());
        auto *upcoming = qobject_cast<TaskListModel *>(widget.upcomingModel());
        QVERIFY(today && upcoming);
        QCOMPARE(today->rowCount(), 0);

        const QDate day = QDate::currentDate();
        const QDateTime todayDue(day, QTime(18, 0), Qt::LocalTime);
        const QDateTime tomorrowDue(day.addDays(2), QTime(18, 0), Qt::LocalTime);

        // created in the main model -> shown in the widget views at once
        QVERIFY(model.addTask("due today", "", "", 2, todayDue));
        QVERIFY(model.addTask("due later", "", "", 0, tomorrowDue));
        QVERIFY(model.addTask("no date"));
        QCOMPARE(today->rowCount(), 1);
        QCOMPARE(upcoming->rowCount(), 1);
        QCOMPARE(today->data(today->index(0), TaskListModel::TitleRole).toString(), QString("due today"));
        QCOMPARE(today->data(today->index(0), TaskListModel::PriorityRole).toInt(), 2);

        // completed through the main model (what the widget's checkbox calls)
        const qint64 id = today->data(today->index(0), TaskListModel::IdRole).toLongLong();
        QVERIFY(model.setCompleted(id, true));
        QCOMPARE(today->rowCount(), 0);
        QVERIFY(model.setCompleted(id, false));
        QCOMPARE(today->rowCount(), 1);

        // re-timed in the main model: moves between the two views
        QVERIFY(model.updateTask(id, "due today", "", "Inbox", 2, tomorrowDue));
        QCOMPARE(today->rowCount(), 0);
        QCOMPARE(upcoming->rowCount(), 2);

        // deleted in the main model, and brought back by undo
        QVERIFY(model.removeTask(id));
        QCOMPARE(upcoming->rowCount(), 1);
        QVERIFY(model.undoDelete());
        QCOMPARE(upcoming->rowCount(), 2);

        // the widget views never write: rows can only change through the main model
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 3);
    }

    void widget_taskCompletedInTheWidgetIsConsistentEverywhere()
    {
        const QString file = path("widget_complete.db");
        Database db;
        QVERIFY(db.initialize(file));
        TaskRepository repo(db);
        TaskListModel model(repo);
        WidgetController widget(model, repo);
        widget.refreshViews();
        auto *today = qobject_cast<TaskListModel *>(widget.todayModel());

        const QDateTime due(QDate::currentDate(), QTime(23, 59), Qt::LocalTime);
        QVERIFY(model.addTask("widget task", "", "", 0, due));
        const qint64 id = model.data(model.index(0), TaskListModel::IdRole).toLongLong();
        QCOMPARE(today->rowCount(), 1);

        QSignalSpy mutated(&model, &TaskListModel::tasksMutated);
        QVERIFY(model.setCompleted(id, true));     // widget checkbox -> main model
        QCOMPARE(mutated.size(), 1);
        QCOMPARE(today->rowCount(), 0);
        QVERIFY(model.data(model.index(0), TaskListModel::CompletedRole).toBool());
        QCOMPARE(scalar(file, "SELECT completed FROM tasks").toInt(), 1);

        // creation through the widget path: a quick add with a due date of today
        QVERIFY(model.addTask("added in widget", "", "", 0, due));
        QCOMPARE(today->rowCount(), 1);
        QCOMPARE(model.rowCount(), 2);
    }

    void widget_hidingAndClosingKeepTasksAndReminders()
    {
        SchedulerFixture f(path("widget_life.db"), kStart);
        QVERIFY(f.ok);
        TaskListModel model(f.repo);
        QObject::connect(&model, &TaskListModel::tasksMutated, &f.scheduler,
                         &ReminderScheduler::reschedule);
        QVERIFY(model.addTask("keep me", "", "", 0, QDateTime(), kStart.addSecs(3600)));
        QVERIFY(f.scheduler.isTimerActive());
        const QDateTime armedFor = f.scheduler.nextReminderAt();

        for (int i = 0; i < 5; ++i) { // shown and hidden repeatedly, and finally destroyed
            WidgetController widget(model, f.repo);
            QVERIFY(widget.load());
            widget.refreshViews();
            widget.setWidgetVisible(true);
            widget.setWidgetVisible(false);
            widget.flush();
        }
        QVERIFY(f.scheduler.isTimerActive());
        QCOMPARE(f.scheduler.nextReminderAt(), armedFor);
        QCOMPARE(model.rowCount(), 1);

        // the controller reacts to changes only while it exists
        QVERIFY(model.addTask("after"));
        QCOMPARE(model.rowCount(), 2);
    }

    void widget_taskDataSupportsOpeningATaskInTheMainWindow()
    {
        Database db;
        QVERIFY(db.initialize(path("widget_open.db")));
        TaskRepository repo(db);
        TaskListModel model(repo);
        const QDateTime due(QDate(2031, 5, 6), QTime(7, 45, 30), Qt::LocalTime);
        QVERIFY(model.addTask("open me", "notes here", "Work", 2, due));
        const qint64 id = model.data(model.index(0), TaskListModel::IdRole).toLongLong();

        const QVariantMap data = model.taskData(id);
        QCOMPARE(data.value("title").toString(), QString("open me"));
        QCOMPARE(data.value("notes").toString(), QString("notes here"));
        QCOMPARE(data.value("listName").toString(), QString("Work"));
        QCOMPARE(data.value("priority").toInt(), 2);
        QCOMPARE(data.value("dueAt").toDateTime(), due);
        QVERIFY(!data.value("reminderAt").isValid() || data.value("reminderAt").isNull());

        QSignalSpy errors(&model, &TaskListModel::errorOccurred);
        QVERIFY(model.taskData(987654).isEmpty());
        QCOMPARE(errors.size(), 0); // a vanished task is not an error
    }
};

QTEST_GUILESS_MAIN(TestTaskNest)

#include "tst_tasknest.moc"
