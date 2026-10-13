// Tests for the database migration, TaskRepository and TaskListModel.
// Every test works on a database file inside a temporary directory.

#include "database.h"
#include "task.h"
#include "tasklistmodel.h"
#include "taskrepository.h"

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
};

QTEST_GUILESS_MAIN(TestTaskNest)

#include "tst_tasknest.moc"
