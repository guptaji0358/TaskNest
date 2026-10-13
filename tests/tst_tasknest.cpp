// Tests for the database migration, TaskRepository and TaskListModel.
// Every test works on a database file inside a temporary directory.

#include "database.h"
#include "reminderscheduler.h"
#include "task.h"
#include "tasklistmodel.h"
#include "taskrepository.h"

#include <QAbstractEventDispatcher>
#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
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

    void migration_freshDatabaseIsV2()
    {
        const QString file = path("fresh.db");
        {
            Database db;
            QVERIFY2(db.initialize(file), qPrintable(db.lastError()));
            QCOMPARE(db.schemaVersion(), 2);
        }
        QCOMPARE(scalar(file, "PRAGMA user_version").toInt(), 2);
        QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='schema_version'").toString(),
                 QString("2"));
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM sqlite_master WHERE name='tasks'").toInt(), 1);
        QCOMPARE(scalar(file, "SELECT COUNT(*) FROM tasks").toInt(), 0);
    }

    void migration_v1ToV2PreservesMetadataAndOtherTables()
    {
        const QString file = path("v1.db");
        QStringList seed = v1Statements();
        seed << "CREATE TABLE user_notes (n TEXT)" << "INSERT INTO user_notes VALUES ('precious')";
        QVERIFY(runSql(file, seed));

        {
            Database db;
            QVERIFY2(db.initialize(file), qPrintable(db.lastError()));
            QCOMPARE(db.schemaVersion(), 2);
        }
        QCOMPARE(scalar(file, "PRAGMA user_version").toInt(), 2);
        QCOMPARE(scalar(file, "SELECT value FROM app_metadata WHERE key='schema_version'").toString(),
                 QString("2"));
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
        QCOMPARE(db.schemaVersion(), 2);
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
            QCOMPARE(db.schemaVersion(), 2);
        }
        const QString backup = file + ".v1.bak";
        QVERIFY(QFile::exists(backup));
        QCOMPARE(fileHash(backup), original);                          // byte-identical
        QCOMPARE(scalar(backup, "PRAGMA user_version").toInt(), 1);    // still the old schema
        QCOMPARE(scalar(file, "PRAGMA user_version").toInt(), 2);

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
        QVERIFY(model.addTask("Two", "notes", "Work", 1));

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
            QVERIFY(model.setCompleted(idA, true));
            QCOMPARE(changed.size(), 1);
            QCOMPARE(resets.size(), 0);
            QCOMPARE(removed.size(), 0);
            QVERIFY(model.data(model.index(0), TaskListModel::CompletedRole).toBool());
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
        const qint64 doneId = model.data(model.index(1), TaskListModel::IdRole).toLongLong();
        QVERIFY(model.setCompleted(doneId, true));
        const qint64 tomorrowId = model.data(model.index(2), TaskListModel::IdRole).toLongLong();

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
};

QTEST_GUILESS_MAIN(TestTaskNest)

#include "tst_tasknest.moc"
