// Tests for the database migration, TaskRepository and TaskListModel.
// Every test works on a database file inside a temporary directory.

#include "cleanuppolicy.h"
#include "database.h"
#include "deeplink.h"
#include "globalhotkey.h"
#include "integrationserver.h"
#include "selectioncapture.h"
#include "nativehost.h"
#include "registryutil.h"
#include "reminderscheduler.h"
#include "singleinstance.h"
#include "task.h"
#include "tasklistmodel.h"
#include "taskrepository.h"
#include "testprofile.h"
#include "widgetcontroller.h"

#include <QAbstractEventDispatcher>
#include <QAbstractItemModelTester>
#include <QBuffer>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <thread>
#include <QLockFile>
#include <QProcess>
#include <QCryptographicHash>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QSysInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <chrono>
#include <functional>
#include <future>
#include <QtTest>

#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
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

namespace {

int taskCount(TaskRepository &repo)
{
    QList<Task> all;
    repo.loadTasks({}, &all);
    return static_cast<int>(all.size());
}

struct IntegrationFixture
{
    QString dbFile;
    QString discovery;
    Database db;
    TaskRepository repo;
    TaskListModel model;
    IntegrationServer server;

    IntegrationFixture(const QString &dbPath, const QString &discoveryDir)
        : dbFile(dbPath)
        , discovery(discoveryDir)
        , db(QStringLiteral("integration_") + QString::number(counter()++))
        , repo(db)
        , model(repo)
        , server(model, repo, dbPath)
    {
        db.initialize(dbPath);
        server.setDiscoveryDirectory(discoveryDir);
    }
    int &counter() { static int n = 0; return n; }
};

// Runs a blocking client call on another thread while the event loop serves it.
IntegrationResponse callClient(const QString &op, const QJsonObject &args,
                                      const QString &discoveryFile)
{
    auto future = std::async(std::launch::async, [=]() {
        return IntegrationClient::request(op, args, 4000, discoveryFile);
    });
    while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        QCoreApplication::processEvents();
    return future.get();
}
IntegrationResponse callRaw(const QByteArray &line, const QString &discoveryFile)
{
    auto future = std::async(std::launch::async, [=]() {
        return IntegrationClient::sendRaw(line, discoveryFile, 4000);
    });
    while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        QCoreApplication::processEvents();
    return future.get();
}

QJsonObject request(const QString &token, const QString &op, const QJsonObject &args = {})
{
    QJsonObject o;
    o.insert("v", 1);
    o.insert("token", token);
    o.insert("op", op);
    o.insert("args", args);
    return o;
}
QString codeOf(const QJsonObject &reply)
{
    return reply.value("error").toObject().value("code").toString();
}

QList<qint64> taskIds(TaskRepository &repo)
{
    QList<Task> all;
    repo.loadTasks({}, &all);
    QList<qint64> ids;
    for (const Task &t : all)
        ids << t.id;
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Sits between the native-messaging host and the integration pipe and makes the
// connection misbehave in one controlled way while the REAL server does the work
// (so the database shows what really happened).
class FaultProxy : public QObject
{
public:
    enum class Mode {
        Relay,          // pass everything through
        SlowReply,      // pass the request on, hold the answer for delayMs
        SwallowRequest, // accept the connection, never hand the request to the server
        DropReply,      // pass the request on, never deliver the answer
        CloseOnReply    // pass the request on, hang up instead of delivering the answer
    };
    Mode mode = Mode::Relay;
    int delayMs = 0;
    int forwarded = 0; // requests handed to the real server

    FaultProxy(const QString &proxyPipe, const QString &realPipe, QObject *parent = nullptr)
        : QObject(parent)
        , m_real(realPipe)
    {
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        QLocalServer::removeServer(proxyPipe);
        m_listening = m_server.listen(proxyPipe);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections())
                accept(m_server.nextPendingConnection());
        });
    }
    bool listening() const { return m_listening; }

private:
    void accept(QLocalSocket *client)
    {
        client->setParent(this);
        auto buffer = std::make_shared<QByteArray>();
        auto sent = std::make_shared<bool>(false);
        QPointer<QLocalSocket> guard(client);
        connect(client, &QLocalSocket::readyRead, this, [this, guard, buffer, sent]() {
            if (!guard || *sent)
                return;
            buffer->append(guard->readAll());
            if (!buffer->contains('\n') || mode == Mode::SwallowRequest)
                return;
            *sent = true;
            forward(guard, *buffer);
        });
        connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
    }

    void forward(QPointer<QLocalSocket> client, const QByteArray &line)
    {
        ++forwarded;
        auto *upstream = new QLocalSocket(this);
        auto answer = std::make_shared<QByteArray>();
        connect(upstream, &QLocalSocket::connected, upstream, [upstream, line]() { upstream->write(line); });
        connect(upstream, &QLocalSocket::readyRead, this, [this, upstream, client, answer]() {
            answer->append(upstream->readAll());
            if (!answer->contains('\n'))
                return;
            disconnect(upstream, &QLocalSocket::readyRead, this, nullptr);
            upstream->disconnectFromServer();
            upstream->deleteLater();
            deliver(client, *answer);
        });
        upstream->connectToServer(m_real);
    }

    void deliver(QPointer<QLocalSocket> client, const QByteArray &answer)
    {
        if (!client)
            return;
        switch (mode) {
        case Mode::Relay:
        case Mode::SwallowRequest:
            client->write(answer);
            break;
        case Mode::SlowReply:
            QTimer::singleShot(delayMs, this, [client, answer]() {
                if (client)
                    client->write(answer);
            });
            break;
        case Mode::DropReply:
            break;
        case Mode::CloseOnReply:
            client->disconnectFromServer();
            break;
        }
    }

    QLocalServer m_server;
    QString m_real;
    bool m_listening = false;
};

// Runs NativeHost::handle on another thread while this thread serves the pipes.
QJsonObject hostCall(const QJsonObject &message, const NativeHost::Options &options)
{
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    auto future = std::async(std::launch::async, [payload, options]() {
        return NativeHost::handle(payload, options);
    });
    while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        QCoreApplication::processEvents();
    return future.get();
}

QJsonObject captureMessage(const QString &requestId, const QString &url = QStringLiteral("https://example.com/page"))
{
    return {{"op", "capture"}, {"title", "Fault test"}, {"url", url},
            {"text", "selected words"}, {"request_id", requestId}};
}

// A real integration server, a FaultProxy in front of it and a discovery file
// (with the server's real credential) that points the host at the proxy.
struct ProxyRig
{
    IntegrationFixture f;
    std::unique_ptr<FaultProxy> proxy;
    QString proxyDiscoveryDir;
    NativeHost::Options options;

    ProxyRig(const QString &dbPath, const QString &realDiscoveryDir, const QString &proxyDir,
             const QString &proxyPipe, const QString &proxyDiscoveryDirOverride = QString())
        : f(dbPath, realDiscoveryDir)
        , proxyDiscoveryDir(proxyDiscoveryDirOverride.isEmpty() ? proxyDir : proxyDiscoveryDirOverride)
    {
        f.server.setEnabled(true);
        QFile real(f.server.discoveryFilePath());
        QJsonObject record;
        if (real.open(QIODevice::ReadOnly))
            record = QJsonDocument::fromJson(real.readAll()).object();
        proxy = std::make_unique<FaultProxy>(proxyPipe, record.value("pipe").toString());
        record.insert("pipe", proxyPipe);
        QDir().mkpath(proxyDiscoveryDir);
        QFile out(QDir(proxyDiscoveryDir).filePath(QStringLiteral("integration-proxy.json")));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
        options.discoveryDirectory = proxyDiscoveryDir;
        options.timeoutMs = 3000;
        options.taskNestRunning = []() { return true; };
    }
};

} // namespace

// A disposable separate process that holds a QLockFile (tests/tst_lockowner.cpp).
// Stopped (and killed if need be) when it goes out of scope, even if a test fails.
struct LockOwner
{
    QProcess process;

    bool start(const QString &exePath, const QString &lockPath)
    {
        process.setProgram(exePath);
        process.setArguments({lockPath});
        process.start();
        return process.waitForStarted(10000) && process.waitForReadyRead(10000)
               && process.readAllStandardOutput().contains("ready");
    }
    qint64 pid() const { return process.processId(); }
    void stop() // graceful: the helper unlocks when its stdin closes
    {
        if (process.state() == QProcess::NotRunning)
            return;
        process.closeWriteChannel();
        if (!process.waitForFinished(5000))
            crash();
    }
    void crash() // like a crash: the lock file is left behind
    {
        process.kill();
        process.waitForFinished(5000);
    }
    ~LockOwner() { crash(); }
};

// A scratch TaskNest.exe started with --test-profile; always ended with the test.
struct AppProcess
{
    QProcess process;

    void start(const QString &exe, const QString &profileRoot)
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen")); // no window on the desktop
        process.setProcessEnvironment(env);
        process.setProgram(exe);
        process.setArguments({QStringLiteral("--test-profile"), profileRoot});
        process.start();
    }
    bool running() const { return process.state() == QProcess::Running; }
    // A headless (offscreen) process has no window to close, so ask its own
    // message loop to quit: WM_QUIT to the threads of this child process only.
    bool requestQuit()
    {
#ifdef Q_OS_WIN
        const DWORD pid = static_cast<DWORD>(process.processId());
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;
        THREADENTRY32 entry;
        entry.dwSize = sizeof(entry);
        bool posted = false;
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == pid)
                    posted = PostThreadMessageW(entry.th32ThreadID, WM_QUIT, 0, 0) || posted;
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return posted;
#else
        return false;
#endif
    }
    void end()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }
    ~AppProcess() { end(); }
};

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

    // ---- local integration endpoint -------------------------------------------

    void integration_isOffByDefaultAndTheChoiceIsRemembered()
    {
        const QString dbFile = path(QStringLiteral("int_default.db"));
        const QString dir = path(QStringLiteral("int_default_disc"));
        QString discoveryFile;
        {
            IntegrationFixture f(dbFile, dir);
            QVERIFY(f.server.load());
            QVERIFY(!f.server.enabled());
            QVERIFY(!f.server.listening());
            QVERIFY(!QFileInfo::exists(f.server.discoveryFilePath()));
            // nothing is listening, so a client is told so and nothing is started
            QCOMPARE(callClient("ping", {}, f.server.discoveryFilePath()).errorCode,
                     QStringLiteral("unavailable"));

            f.server.setEnabled(true);
            QVERIFY(f.server.listening());
            QVERIFY(QFileInfo::exists(f.server.discoveryFilePath()));
            QVERIFY(f.server.token().size() == 64);
            discoveryFile = f.server.discoveryFilePath();
        }
        // leaving the scope (shutdown) removed the endpoint
        QVERIFY(!discoveryFile.isEmpty());
        QVERIFY(!QFileInfo::exists(discoveryFile));
        {
            IntegrationFixture f(dbFile, dir);
            QVERIFY(f.server.load());
            QVERIFY(f.server.enabled());        // the stored choice
            QVERIFY(f.server.listening());
            f.server.setEnabled(false);
            QVERIFY(!f.server.listening());
        }
        {
            IntegrationFixture f(dbFile, dir);
            QVERIFY(f.server.load());
            QVERIFY(!f.server.enabled());
            QVERIFY(!f.server.listening());
        }
    }

    void integration_createTaskAndCapturePageUseTheTaskModel()
    {
        IntegrationFixture f(path(QStringLiteral("int_create.db")), path(QStringLiteral("int_create_disc")));
        f.server.setEnabled(true);
        QVERIFY(f.server.listening());
        const QString file = f.server.discoveryFilePath();

        QJsonObject args;
        args.insert("title", "  Buy milk  ");
        args.insert("notes", "2 litres");
        const IntegrationResponse r = callClient("create_task", args, file);
        QVERIFY2(r.ok, qPrintable(r.errorCode + r.errorMessage));
        const qint64 id = static_cast<qint64>(r.result.value("task_id").toDouble());
        QVERIFY(id > 0);
        Task stored;
        QVERIFY(f.repo.getTask(id, &stored));
        QCOMPARE(stored.title, QStringLiteral("Buy milk"));
        QCOMPARE(stored.notes, QStringLiteral("2 litres"));
        QVERIFY(!stored.completed);

        QJsonObject page;
        page.insert("title", "An article");
        page.insert("url", "https://example.com/a/b?x=1#frag");
        page.insert("notes", "read later");
        const IntegrationResponse p = callClient("capture_page", page, file);
        QVERIFY2(p.ok, qPrintable(p.errorCode + p.errorMessage));
        Task captured;
        QVERIFY(f.repo.getTask(static_cast<qint64>(p.result.value("task_id").toDouble()), &captured));
        QCOMPARE(captured.title, QStringLiteral("An article"));
        QVERIFY(captured.notes.startsWith(QStringLiteral("read later")));
        QVERIFY(captured.notes.endsWith(QStringLiteral("Source: https://example.com/a/b?x=1#frag")));

        // without a page title the address names the task
        QJsonObject bare;
        bare.insert("url", "http://example.org/path");
        const IntegrationResponse b = callClient("capture_page", bare, file);
        QVERIFY(b.ok);
        Task bareTask;
        QVERIFY(f.repo.getTask(static_cast<qint64>(b.result.value("task_id").toDouble()), &bareTask));
        QCOMPARE(bareTask.title, QStringLiteral("example.org/path"));

        // repeated identical requests make separate tasks (documented, not deduplicated)
        const IntegrationResponse r2 = callClient("create_task", args, file);
        QVERIFY(r2.ok);
        QVERIFY(r2.result.value("task_id").toDouble() != r.result.value("task_id").toDouble());
    }

    void integration_viewTaskAndQuickAddRaiseUiRequests()
    {
        IntegrationFixture f(path(QStringLiteral("int_ui.db")), path(QStringLiteral("int_ui_disc")));
        f.server.setEnabled(true);
        const QString file = f.server.discoveryFilePath();
        QVERIFY(f.model.addTask(QStringLiteral("existing")));
        const qint64 id = f.model.lastAddedId();

        QSignalSpy viewSpy(&f.server, &IntegrationServer::openViewRequested);
        QSignalSpy taskSpy(&f.server, &IntegrationServer::openTaskRequested);
        QSignalSpy addSpy(&f.server, &IntegrationServer::quickAddRequested);

        QJsonObject v;
        v.insert("view", "today");
        QVERIFY(callClient("open_view", v, file).ok);
        QCOMPARE(viewSpy.count(), 1);
        QCOMPARE(viewSpy.first().first().toString(), QStringLiteral("today"));

        v.insert("view", "nonsense");
        QCOMPARE(callClient("open_view", v, file).errorCode, QStringLiteral("invalid_argument"));
        QCOMPARE(viewSpy.count(), 1);

        QJsonObject t;
        t.insert("task_id", static_cast<double>(id));
        QVERIFY(callClient("open_task", t, file).ok);
        QCOMPARE(taskSpy.count(), 1);
        QCOMPARE(taskSpy.first().first().toLongLong(), id);

        t.insert("task_id", static_cast<double>(id + 1000));
        QCOMPARE(callClient("open_task", t, file).errorCode, QStringLiteral("not_found"));
        QCOMPARE(taskSpy.count(), 1);

        QVERIFY(callClient("quick_add", {}, file).ok);
        QCOMPARE(addSpy.count(), 1);
    }

    void integration_rejectsMalformedUnauthorizedAndUnsupportedRequests()
    {
        IntegrationFixture f(path(QStringLiteral("int_bad.db")), path(QStringLiteral("int_bad_disc")));
        f.server.setEnabled(true);
        const QString tok = f.server.token();
        QVERIFY(f.model.addTask(QStringLiteral("seed")));
        const int before = taskCount(f.repo);

        auto line = [&](const QByteArray &bytes) {
            return QJsonDocument::fromJson(f.server.handleLine(bytes)).object();
        };
        QCOMPARE(codeOf(line("not json")), QStringLiteral("bad_request"));
        QCOMPARE(codeOf(line("[1,2]")), QStringLiteral("bad_request"));
        QCOMPARE(codeOf(line("")), QStringLiteral("bad_request"));
        QCOMPARE(codeOf(line(QByteArray(IntegrationServer::kMaxMessageBytes + 10, 'a'))),
                 QStringLiteral("too_large"));

        QJsonObject noToken;
        noToken.insert("v", 1);
        noToken.insert("op", "ping");
        QCOMPARE(codeOf(f.server.handle(noToken)), QStringLiteral("unauthorized"));
        QCOMPARE(codeOf(f.server.handle(request("0000", "ping"))), QStringLiteral("unauthorized"));
        // a nearly-right token is still wrong
        QCOMPARE(codeOf(f.server.handle(request(tok.left(63) + "x", "ping"))),
                 QStringLiteral("unauthorized"));

        QJsonObject wrongVersion = request(tok, "ping");
        wrongVersion.insert("v", 2);
        QCOMPARE(codeOf(f.server.handle(wrongVersion)), QStringLiteral("unsupported_version"));

        QCOMPARE(codeOf(f.server.handle(request(tok, "delete_everything"))),
                 QStringLiteral("unknown_operation"));

        QJsonObject extraTop = request(tok, "ping");
        extraTop.insert("sql", "DROP TABLE tasks");
        QCOMPARE(codeOf(f.server.handle(extraTop)), QStringLiteral("unknown_field"));

        auto create = [&](const QJsonObject &args) { return f.server.handle(request(tok, "create_task", args)); };
        QJsonObject a;
        QCOMPARE(codeOf(create(a)), QStringLiteral("invalid_argument"));           // no title
        a.insert("title", "   ");
        QCOMPARE(codeOf(create(a)), QStringLiteral("invalid_argument"));           // blank
        a.insert("title", 5);
        QCOMPARE(codeOf(create(a)), QStringLiteral("invalid_argument"));           // wrong type
        a.insert("title", "line\nbreak");
        QCOMPARE(codeOf(create(a)), QStringLiteral("invalid_argument"));           // control char
        a.insert("title", QString(IntegrationServer::kMaxTitleChars + 1, 'x'));
        QCOMPARE(codeOf(create(a)), QStringLiteral("invalid_argument"));           // too long
        a.insert("title", "ok");
        a.insert("priority", 2);
        QCOMPARE(codeOf(create(a)), QStringLiteral("unknown_field"));              // unsupported arg
        a.remove("priority");
        a.insert("notes", QString(IntegrationServer::kMaxNotesChars + 1, 'n'));
        QCOMPARE(codeOf(create(a)), QStringLiteral("invalid_argument"));

        auto capture = [&](const QString &url) {
            QJsonObject c;
            c.insert("url", url);
            return f.server.handle(request(tok, "capture_page", c));
        };
        QCOMPARE(codeOf(capture("javascript:alert(1)")), QStringLiteral("invalid_argument"));
        QCOMPARE(codeOf(capture("file:///C:/Windows/win.ini")), QStringLiteral("invalid_argument"));
        QCOMPARE(codeOf(capture("https://")), QStringLiteral("invalid_argument"));
        QCOMPARE(codeOf(capture("not a url")), QStringLiteral("invalid_argument"));
        QCOMPARE(codeOf(capture("https://example.com/" + QString(3000, 'a'))),
                 QStringLiteral("invalid_argument"));

        for (const QJsonValue &bad : {QJsonValue(0), QJsonValue(-3), QJsonValue(1.5),
                                      QJsonValue("1"), QJsonValue(1e300), QJsonValue()}) {
            QJsonObject t;
            t.insert("task_id", bad);
            QCOMPARE(codeOf(f.server.handle(request(tok, "open_task", t))),
                     QStringLiteral("invalid_argument"));
        }
        QJsonObject ping;
        ping.insert("x", 1);
        QCOMPARE(codeOf(f.server.handle(request(tok, "ping", ping))), QStringLiteral("unknown_field"));

        QCOMPARE(taskCount(f.repo), before); // nothing above created a task
    }

    void integration_oversizedAndGarbageOverTheSocketAreAnsweredSafely()
    {
        IntegrationFixture f(path(QStringLiteral("int_sock.db")), path(QStringLiteral("int_sock_disc")));
        f.server.setEnabled(true);
        const QString file = f.server.discoveryFilePath();
        const int before = taskCount(f.repo);

        const IntegrationResponse big =
            callRaw(QByteArray(IntegrationServer::kMaxMessageBytes + 100, 'a') + '\n', file);
        QCOMPARE(big.errorCode, QStringLiteral("too_large"));

        const IntegrationResponse garbage = callRaw("}{ definitely not json\n", file);
        QCOMPARE(garbage.errorCode, QStringLiteral("bad_request"));

        // the endpoint still works afterwards
        QVERIFY(callClient("ping", {}, file).ok);
        QCOMPARE(taskCount(f.repo), before);
    }

    void integration_shutdownRemovesTheEndpointAndATokenIsNotReused()
    {
        IntegrationFixture f(path(QStringLiteral("int_stop.db")), path(QStringLiteral("int_stop_disc")));
        f.server.setEnabled(true);
        const QString file = f.server.discoveryFilePath();
        const QString firstToken = f.server.token();
        QVERIFY(callClient("ping", {}, file).ok);

        f.server.stop();
        QVERIFY(!f.server.listening());
        QVERIFY(!QFileInfo::exists(file));
        QCOMPARE(callClient("ping", {}, file).errorCode, QStringLiteral("unavailable"));
        f.server.stop(); // idempotent

        f.server.setEnabled(false);
        f.server.setEnabled(true);
        QVERIFY(f.server.listening());
        QVERIFY(f.server.token() != firstToken);
        // the token from the previous run no longer authorizes anything
        QCOMPARE(codeOf(f.server.handle(request(firstToken, "ping"))), QStringLiteral("unauthorized"));
        QVERIFY(callClient("ping", {}, file).ok);
    }

    void integration_staleDiscoveryFileMeansUnavailable()
    {
        const QString dir = path(QStringLiteral("int_stale_disc"));
        QDir().mkpath(dir);
        const QString stale = dir + "/integration-dead.json";
        QFile file(stale);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"protocol\":1,\"pipe\":\"TaskNest-nobody-listens\",\"token\":\"x\",\"pid\":1}");
        file.close();
        const IntegrationResponse r = callClient("ping", {}, stale);
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, QStringLiteral("unavailable"));
        // a corrupt file is also just "unavailable"
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("garbage");
        file.close();
        QCOMPARE(callClient("ping", {}, stale).errorCode, QStringLiteral("unavailable"));
    }

    // ---- deep links and the global shortcut ---------------------------------

    void deeplink_parsesTheSupportedForms()
    {
        DeepLink t = DeepLink::parse(QStringLiteral("tasknest://task/42"));
        QCOMPARE(t.kind, DeepLink::OpenTask);
        QCOMPARE(t.taskId, qint64(42));
        t = DeepLink::parse(QStringLiteral("TaskNest://TASK/7"));   // scheme and host are case-insensitive
        QCOMPARE(t.kind, DeepLink::OpenTask);
        QCOMPARE(t.taskId, qint64(7));
        t = DeepLink::parse(QStringLiteral("  tasknest://task/999999999999999  "));
        QCOMPARE(t.taskId, qint64(999999999999999));

        for (const QString &view : DeepLink::views()) {
            const DeepLink v = DeepLink::parse(QStringLiteral("tasknest://view/") + view);
            QCOMPARE(v.kind, DeepLink::OpenView);
            QCOMPARE(v.view, view);
        }
        QCOMPARE(DeepLink::parse(QStringLiteral("tasknest://view/Today")).view, QStringLiteral("today"));
        QCOMPARE(DeepLink::parse(QStringLiteral("tasknest://quickadd")).kind, DeepLink::QuickAdd);
        QCOMPARE(DeepLink::parse(QStringLiteral("tasknest://quickadd/")).kind, DeepLink::QuickAdd);
    }

    void deeplink_rejectsEverythingElse()
    {
        const QStringList bad = {
            "", "   ", "tasknest:", "tasknest://", "http://example.com/task/1",
            "tasknest://task", "tasknest://task/", "tasknest://task/0", "tasknest://task/-1",
            "tasknest://task/007", "tasknest://task/1.5", "tasknest://task/abc",
            "tasknest://task/1/2", "tasknest://task/9999999999999999",  // 16 digits
            "tasknest://task/1?x=1", "tasknest://task/1#frag",
            "tasknest://view", "tasknest://view/nonsense", "tasknest://view/today/extra",
            "tasknest://view/..%2f..", "tasknest://delete/1", "tasknest://create?title=x",
            "tasknest://quickadd/now", "tasknest://quickadd?title=secret",
            "tasknest://user@task/1", "tasknest://task:80/1",
            "tasknest://task/1 ; calc.exe", "tasknest://task/1\n\x01",
            "tasknest://run/C:/Windows/System32/cmd.exe",
        };
        for (const QString &text : bad) {
            const DeepLink link = DeepLink::parse(text);
            QVERIFY2(!link.isValid(), qPrintable(QStringLiteral("accepted: ") + text));
            QVERIFY(!link.error.isEmpty());
            // the reason never repeats the link text
            if (text.trimmed().size() > 6)
                QVERIFY(!link.error.contains(text.trimmed()));
        }
        QVERIFY(!DeepLink::parse(QStringLiteral("tasknest://task/") + QString(300, QLatin1Char('1'))).isValid());
    }

    void deeplink_findsTheLinkInWindowsArguments()
    {
        const QString exe = QStringLiteral("C:/Program Files/TaskNest/TaskNest.exe");
        QCOMPARE(DeepLink::fromArguments({exe}), QString());
        QCOMPARE(DeepLink::fromArguments({exe, "tasknest://task/5"}), QStringLiteral("tasknest://task/5"));
        // other switches are ignored; only the first link counts
        QCOMPARE(DeepLink::fromArguments({exe, "--style=fusion", "TaskNest://view/today", "tasknest://task/9"}),
                 QStringLiteral("TaskNest://view/today"));
        QCOMPARE(DeepLink::fromArguments({exe, "C:/files/tasknest.txt", "-platform", "windows"}), QString());
        // the executable path itself is never mistaken for a link
        QCOMPARE(DeepLink::fromArguments({QStringLiteral("tasknest://task/1")}), QString());
        QCOMPARE(UrlSchemeRegistration::commandFor("C:/Program Files/TaskNest/TaskNest.exe"),
                 QStringLiteral("\"C:\\Program Files\\TaskNest\\TaskNest.exe\" \"%1\""));
    }

    void deeplink_routerEmitsOnlyForValidLinks()
    {
        DeepLinkRouter router;
        QSignalSpy views(&router, &DeepLinkRouter::openViewRequested);
        QSignalSpy tasks(&router, &DeepLinkRouter::openTaskRequested);
        QSignalSpy quick(&router, &DeepLinkRouter::quickAddRequested);
        QSignalSpy rejected(&router, &DeepLinkRouter::rejected);

        QVERIFY(router.handleUrl("tasknest://view/upcoming"));
        QVERIFY(router.handleUrl("tasknest://task/12"));   // an id that may not exist: the UI says so
        QVERIFY(router.handleUrl("tasknest://quickadd"));
        QVERIFY(!router.handleUrl("tasknest://view/secret-project-name"));
        QVERIFY(!router.handleUrl("tasknest://task/1?title=private"));
        QCOMPARE(views.count(), 1);
        QCOMPARE(views.first().first().toString(), QStringLiteral("upcoming"));
        QCOMPARE(tasks.count(), 1);
        QCOMPARE(tasks.first().first().toLongLong(), qint64(12));
        QCOMPARE(quick.count(), 1);
        QCOMPARE(rejected.count(), 2);
        for (const auto &args : rejected) {
            QVERIFY(!args.first().toString().contains("secret-project-name"));
            QVERIFY(!args.first().toString().contains("private"));
        }
    }

#ifdef Q_OS_WIN
    // The receiving half of "second launch forwards its link to the first":
    // a WM_COPYDATA message with our tag. (The sending half and the real
    // cross-process delivery are exercised against the built executable.)
    void deeplink_copyDataFromAnotherLaunchIsRoutedAndValidated()
    {
        DeepLinkRouter router;
        QSignalSpy tasks(&router, &DeepLinkRouter::openTaskRequested);
        QSignalSpy rejected(&router, &DeepLinkRouter::rejected);

        auto deliver = [&router](quintptr tag, const QString &text, bool oddLength = false) {
            QByteArray bytes(reinterpret_cast<const char *>(text.utf16()),
                             static_cast<int>(text.size() * 2));
            if (oddLength)
                bytes.chop(1);
            COPYDATASTRUCT cds;
            cds.dwData = tag;
            cds.cbData = static_cast<DWORD>(bytes.size());
            cds.lpData = bytes.data();
            MSG msg = {};
            msg.hwnd = reinterpret_cast<HWND>(static_cast<quintptr>(0x1234));
            msg.message = WM_COPYDATA;
            msg.lParam = reinterpret_cast<LPARAM>(&cds);
            qintptr result = 0;
            const bool handled = router.nativeEventFilter("windows_generic_MSG", &msg, &result);
            return std::make_pair(handled, result);
        };

        auto ok = deliver(DeepLinkRouter::kCopyDataId, "tasknest://task/31");
        QVERIFY(ok.first);
        QCOMPARE(ok.second, qintptr(1));
        QTRY_COMPARE(tasks.count(), 1);
        QCOMPARE(tasks.first().first().toLongLong(), qint64(31));

        // Someone else's WM_COPYDATA is left alone.
        QVERIFY(!deliver(0xABCDEF, "tasknest://task/32").first);
        // Malformed payloads reach the validator and are refused, not executed.
        QVERIFY(deliver(DeepLinkRouter::kCopyDataId, "calc.exe").first);
        QVERIFY(deliver(DeepLinkRouter::kCopyDataId, "tasknest://task/1", true).first);
        QVERIFY(deliver(DeepLinkRouter::kCopyDataId, QString(2000, QLatin1Char('a'))).first);
        QTRY_COMPARE(rejected.count(), 3);
        QCOMPARE(tasks.count(), 1);

        // No such process: nothing is delivered and nothing blocks.
        QVERIFY(!SingleInstanceGuard::sendDataToProcess(0, DeepLinkRouter::kCopyDataId, "x"));
        QVERIFY(!SingleInstanceGuard::sendDataToProcess(4000000000LL, DeepLinkRouter::kCopyDataId, "x"));
    }

    void deeplink_registrationUsesAScratchKeyAndCleansUp()
    {
        const QString root = QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTests\\Classes%1")
                                 .arg(QCoreApplication::applicationPid());
        const QString exe = QStringLiteral("C:/Program Files/TaskNest/TaskNest.exe");
        {
            UrlSchemeRegistration reg(root);
            QVERIFY(!reg.isRegistered(exe));
            // asking creates nothing in the registry
            QVERIFY(!RegistryUtil::keyExists(root + "\\tasknest"));
            QVERIFY(!RegistryUtil::keyExists(root));
            QVERIFY(reg.registerScheme(exe));
            QVERIFY(reg.isRegistered(exe));
            QVERIFY(reg.registerScheme(exe)); // repeating is harmless: no duplicate state
            QVERIFY(reg.isRegistered(exe));
            QVERIFY(!reg.isRegistered(QStringLiteral("C:/Elsewhere/Other.exe"))); // another exe is not "us"
            QVERIFY(reg.unregisterScheme());
            QVERIFY(!reg.isRegistered(exe));
            QVERIFY(!RegistryUtil::keyExists(root + "\\tasknest")); // no empty key left behind
            QVERIFY(reg.unregisterScheme()); // removing what is not there is fine
        }
        // remove the scratch parent key as well
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTests"),
                  QSettings::NativeFormat).remove(QString());
    }

    // Real RegisterHotKey calls with an unusual combination, released again.
    void hotkey_registrationConflictFailureAndCleanup()
    {
        Database db(QStringLiteral("hotkey_conflict"));
        QVERIFY(db.initialize(path(QStringLiteral("hotkey.db"))));
        TaskRepository repo(db);

        GlobalHotkey first(repo);
        GlobalHotkey second(repo);
        const QString combo = QStringLiteral("Ctrl+Alt+Shift+F11");

        first.setShortcut(combo);
        first.setEnabled(true);
        QVERIFY2(first.isRegistered(), qPrintable(first.lastError()));
        QVERIFY(first.lastError().isEmpty());

        // The same combination again: refused with a useful message, no crash.
        second.setShortcut(combo);
        second.setEnabled(true);
        QVERIFY(!second.isRegistered());
        QVERIFY2(second.lastError().contains("already used"), qPrintable(second.lastError()));
        QVERIFY(second.enabled()); // the choice stays; only the registration failed

        // Releasing the first frees the combination for the second.
        first.setEnabled(false);
        QVERIFY(!first.isRegistered());
        second.setEnabled(false);
        second.setEnabled(true);
        QVERIFY2(second.isRegistered(), qPrintable(second.lastError()));
        second.setEnabled(false);
        QVERIFY(!second.isRegistered());
        QVERIFY(second.lastError().isEmpty());
    }

    void hotkey_destructionReleasesTheRegistration()
    {
        Database db(QStringLiteral("hotkey_cleanup"));
        QVERIFY(db.initialize(path(QStringLiteral("hotkey2.db"))));
        TaskRepository repo(db);
        const QString combo = QStringLiteral("Ctrl+Alt+Shift+F10");
        {
            GlobalHotkey a(repo);
            a.setShortcut(combo);
            a.setEnabled(true);
            QVERIFY2(a.isRegistered(), qPrintable(a.lastError()));
        }
        GlobalHotkey b(repo);
        b.setShortcut(combo);
        b.setEnabled(true);
        QVERIFY2(b.isRegistered(), qPrintable(b.lastError())); // a's registration was released
    }
#endif

    void hotkey_parsesAndRefusesUnsupportedCombinations()
    {
        unsigned mods = 0, vk = 0;
        QVERIFY(GlobalHotkey::parse("Ctrl+Alt+N", &mods, &vk));
        QCOMPARE(vk, unsigned('N'));
        QVERIFY(GlobalHotkey::parse("ctrl+shift+space", &mods, &vk));
        QCOMPARE(vk, unsigned(0x20));
        QVERIFY(GlobalHotkey::parse("Alt+Shift+F9", &mods, &vk));
        QCOMPARE(vk, unsigned(0x70 + 8));
        for (const QString &c : GlobalHotkey::defaultChoices())
            QVERIFY2(GlobalHotkey::parse(c, nullptr, nullptr), qPrintable(c));

        const QStringList bad = {"", "N", "Ctrl+N", "Alt+N", "Shift+N", "Shift+Alt+", "Ctrl+Alt",
                                 "Ctrl+Ctrl+N", "Ctrl+Alt+F13", "Ctrl+Alt+F0", "Ctrl+Alt+Enter",
                                 "Ctrl+Alt+Delete", "Win+Shift+N", "Ctrl+Win+N", "Ctrl+Foo+N",
                                 "Ctrl+Alt+NN", "Ctrl+Alt+%", "Ctrl+Shift+Z+Y"};
        for (const QString &text : bad) {
            QString why;
            QVERIFY2(!GlobalHotkey::parse(text, nullptr, nullptr, &why), qPrintable(text));
            QVERIFY(!why.isEmpty());
        }
    }

    void hotkey_isOffByDefaultAndTheChoiceIsRemembered()
    {
        const QString file = path(QStringLiteral("hotkey_prefs.db"));
        {
            Database db(QStringLiteral("hotkey_prefs1"));
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            GlobalHotkey h(repo);
            QVERIFY(h.load());
            QVERIFY(!h.enabled());              // off until the user turns it on
            QVERIFY(!h.isRegistered());
            QCOMPARE(h.shortcut(), GlobalHotkey::defaultShortcut());
            h.setShortcut("Ctrl+Alt+T");        // choosing does not enable it
            QVERIFY(!h.enabled());
            QVERIFY(!h.isRegistered());
            h.setShortcut("Ctrl+N");            // unsupported: refused, old choice kept
            QCOMPARE(h.shortcut(), QStringLiteral("Ctrl+Alt+T"));
            QVERIFY(!h.lastError().isEmpty());
        }
        {
            Database db(QStringLiteral("hotkey_prefs2"));
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            GlobalHotkey h(repo);
            QVERIFY(h.load());
            QVERIFY(!h.enabled());
            QCOMPARE(h.shortcut(), QStringLiteral("Ctrl+Alt+T")); // remembered
        }
        // an unsupported stored shortcut is not used
        QVERIFY(runSql(file, {QStringLiteral("INSERT OR REPLACE INTO app_metadata (key, value) VALUES "
                                             "('global_quick_add', '{\"enabled\":true,\"shortcut\":\"Ctrl+N\"}')")}));
        {
            Database db(QStringLiteral("hotkey_prefs3"));
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            GlobalHotkey h(repo);
            QVERIFY(h.load());
            QCOMPARE(h.shortcut(), GlobalHotkey::defaultShortcut()); // the bad value is not used
            h.setEnabled(false);
        }
    }

    // ---- request ids (capture_page idempotency) -------------------------------

    void integration_requestIdMakesARetryNotADuplicate()
    {
        IntegrationFixture f(path(QStringLiteral("int_rid.db")), path(QStringLiteral("int_rid_disc")));
        f.server.setEnabled(true);
        const QString tok = f.server.token();
        auto capture = [&](const QString &id, const QString &title = QStringLiteral("Page")) {
            QJsonObject a;
            a.insert("url", "https://example.com/p");
            a.insert("title", title);
            if (!id.isEmpty())
                a.insert("request_id", id);
            return f.server.handle(request(tok, "capture_page", a));
        };
        const int before = taskCount(f.repo);

        const QJsonObject first = capture("req-0001-abcdef");
        QVERIFY(first.value("ok").toBool());
        QVERIFY(!first.value("result").toObject().contains("duplicate"));
        const QJsonObject retry = capture("req-0001-abcdef");
        QVERIFY(retry.value("ok").toBool());
        QVERIFY(retry.value("result").toObject().value("duplicate").toBool());
        QCOMPARE(retry.value("result").toObject().value("task_id"),
                 first.value("result").toObject().value("task_id"));
        QCOMPARE(taskCount(f.repo), before + 1);

        // A different id, or no id at all, is always a new task: no global dedup.
        QVERIFY(capture("req-0002-abcdef").value("ok").toBool());
        QVERIFY(capture("").value("ok").toBool());
        QVERIFY(capture("").value("ok").toBool());
        QCOMPARE(taskCount(f.repo), before + 4);

        // malformed ids are refused and create nothing
        for (const QString &bad : {QStringLiteral("short"), QStringLiteral("has space in it!"),
                                   QString(65, QLatin1Char('a')), QStringLiteral("../../etc/passwd")}) {
            QCOMPARE(codeOf(capture(bad)), QStringLiteral("invalid_argument"));
        }
        QCOMPARE(taskCount(f.repo), before + 4);

        // ids are forgotten when the endpoint stops (in-memory only)
        f.server.setEnabled(false);
        f.server.setEnabled(true);
        QVERIFY(!f.server.handle(request(f.server.token(), "capture_page",
                    QJsonObject{{"url", "https://example.com/p"}, {"request_id", "req-0001-abcdef"}}))
                     .value("result").toObject().contains("duplicate"));
    }

    // ---- native messaging host -----------------------------------------------

    void nativehost_framesRoundTripAndRefuseBadInput()
    {
        const QByteArray json = QByteArray("{\"op\":\"status\",\"x\":\"\xC3\xA9\xE2\x82\xAC\"}");
        const QByteArray frame = NativeHost::encodeFrame(json);
        QCOMPARE(frame.size(), json.size() + 4);
        QCOMPARE(quint8(frame[0]), quint8(json.size() & 0xFF)); // little-endian length
        QCOMPARE(quint8(frame[1]), quint8((json.size() >> 8) & 0xFF));

        QBuffer ok(const_cast<QByteArray *>(&frame));
        QVERIFY(ok.open(QIODevice::ReadOnly));
        QByteArray payload;
        QCOMPARE(NativeHost::readFrame(ok, &payload), NativeHost::FrameStatus::Ok);
        QCOMPARE(payload, json);

        auto statusOf = [](const QByteArray &bytes) {
            QByteArray copy = bytes;
            QBuffer b(&copy);
            b.open(QIODevice::ReadOnly);
            QByteArray p;
            return NativeHost::readFrame(b, &p);
        };
        QCOMPARE(statusOf(QByteArray()), NativeHost::FrameStatus::Eof);
        QCOMPARE(statusOf(QByteArray("\x05\x00", 2)), NativeHost::FrameStatus::Truncated);          // short header
        QCOMPARE(statusOf(QByteArray("\x0A\x00\x00\x00{\"a\"", 8)), NativeHost::FrameStatus::Truncated); // short body
        QCOMPARE(statusOf(QByteArray("\x00\x00\x00\x00", 4)), NativeHost::FrameStatus::Empty);
        // a length over the limit is refused before any body is read
        QByteArray huge("\xFF\xFF\xFF\x7F", 4);
        huge.append(QByteArray(100, 'a'));
        QCOMPARE(statusOf(huge), NativeHost::FrameStatus::TooLarge);
        const quint32 limit = NativeHost::kMaxRequestBytes + 1;
        QByteArray over;
        over.append(char(limit & 0xFF)).append(char((limit >> 8) & 0xFF))
            .append(char((limit >> 16) & 0xFF)).append(char((limit >> 24) & 0xFF));
        over.append(QByteArray(limit, 'a'));
        QCOMPARE(statusOf(over), NativeHost::FrameStatus::TooLarge);
    }

    void nativehost_runAnswersOnceAndHandlesBrokenInput()
    {
        NativeHost::Options options;
        options.discoveryDirectory = path(QStringLiteral("nh_run_empty"));
        options.taskNestRunning = []() { return false; };

        auto runWith = [&options](const QByteArray &input, QByteArray *output) {
            QByteArray in = input;
            QBuffer inDevice(&in);
            inDevice.open(QIODevice::ReadOnly);
            QBuffer outDevice(output);
            outDevice.open(QIODevice::WriteOnly);
            return NativeHost::run(inDevice, outDevice, options);
        };
        auto decode = [](const QByteArray &out) {
            QByteArray copy = out;
            QBuffer b(&copy);
            b.open(QIODevice::ReadOnly);
            QByteArray p;
            if (NativeHost::readFrame(b, &p) != NativeHost::FrameStatus::Ok)
                return QJsonObject();
            return QJsonDocument::fromJson(p).object();
        };

        QByteArray out;
        QCOMPARE(runWith(NativeHost::encodeFrame("{\"op\":\"status\"}"), &out), 0);
        QCOMPARE(codeOf(decode(out)), QStringLiteral("not_running"));

        out.clear();
        QCOMPARE(runWith(QByteArray("\x07\x00\x00\x00{\"op\"", 8), &out), 0); // truncated body
        QCOMPARE(codeOf(decode(out)), QStringLiteral("bad_message"));

        out.clear();
        QCOMPARE(runWith(QByteArray(), &out), 1); // nothing to read: no reply, exit code 1
        QVERIFY(out.isEmpty());

        out.clear();
        QCOMPARE(runWith(QByteArray("\xFF\xFF\xFF\x7F", 4), &out), 0);
        QCOMPARE(codeOf(decode(out)), QStringLiteral("too_large"));
    }

    void nativehost_validatesMessagesBeforeAnyConnection()
    {
        NativeHost::Options options;
        options.discoveryDirectory = path(QStringLiteral("nh_validate_empty"));
        options.taskNestRunning = []() { return false; };
        auto code = [&options](const QByteArray &json) {
            return codeOf(NativeHost::handle(json, options));
        };
        QCOMPARE(code("not json"), QStringLiteral("bad_message"));
        QCOMPARE(code("[]"), QStringLiteral("bad_message"));
        QCOMPARE(code("{}"), QStringLiteral("bad_message"));
        QCOMPARE(code("{\"op\":5}"), QStringLiteral("bad_message"));
        QCOMPARE(code("{\"op\":\"open_view\"}"), QStringLiteral("unsupported_operation"));
        QCOMPARE(code("{\"op\":\"create_task\",\"title\":\"x\"}"), QStringLiteral("unsupported_operation"));
        QCOMPARE(code("{\"op\":\"capture_page\"}"), QStringLiteral("unsupported_operation"));
        QCOMPARE(code("{\"op\":\"status\",\"pipe\":\"TaskNest-0000000000000000\"}"), QStringLiteral("bad_message"));
        // a pipe, a path or a token can never be supplied by the message
        QCOMPARE(code("{\"op\":\"capture\",\"text\":\"a\",\"url\":\"https://a.b/\",\"pipe\":\"x\"}"),
                 QStringLiteral("bad_message"));
        QCOMPARE(code("{\"op\":\"capture\",\"text\":\"a\",\"url\":\"https://a.b/\",\"token\":\"x\"}"),
                 QStringLiteral("bad_message"));
        QCOMPARE(code("{\"op\":\"capture\",\"text\":\"a\",\"url\":\"https://a.b/\",\"discovery\":\"C:/x\"}"),
                 QStringLiteral("bad_message"));
        // content checks
        QCOMPARE(code("{\"op\":\"capture\",\"url\":\"https://a.b/\"}"), QStringLiteral("invalid_argument"));
        QCOMPARE(code("{\"op\":\"capture\",\"text\":\"   \\n\\t \",\"url\":\"https://a.b/\"}"),
                 QStringLiteral("invalid_argument"));
        QCOMPARE(code("{\"op\":\"capture\",\"text\":5,\"url\":\"https://a.b/\"}"), QStringLiteral("invalid_argument"));
        QCOMPARE(code("{\"op\":\"capture\",\"text\":\"a\"}"), QStringLiteral("invalid_argument"));
        QJsonObject longText{{"op", "capture"}, {"url", "https://a.b/"},
                             {"text", QString(NativeHost::kMaxTextChars + 1, QLatin1Char('x'))}};
        QCOMPARE(code(QJsonDocument(longText).toJson()), QStringLiteral("invalid_argument"));
        QJsonObject longTitle{{"op", "capture"}, {"url", "https://a.b/"}, {"text", "ok"},
                              {"title", QString(NativeHost::kMaxTitleChars + 1, QLatin1Char('t'))}};
        QCOMPARE(code(QJsonDocument(longTitle).toJson()), QStringLiteral("invalid_argument"));
        QJsonObject longUrl{{"op", "capture"}, {"text", "ok"},
                            {"url", "https://a.b/" + QString(NativeHost::kMaxUrlChars, QLatin1Char('p'))}};
        QCOMPARE(code(QJsonDocument(longUrl).toJson()), QStringLiteral("invalid_argument"));
        // a message that passes the host's checks but finds no endpoint
        QCOMPARE(code("{\"op\":\"capture\",\"text\":\"hello\",\"url\":\"https://a.b/\"}"),
                 QStringLiteral("not_running"));
    }

    void nativehost_reportsDisabledNotRunningAndBadDiscovery()
    {
        const QString dir = path(QStringLiteral("nh_state"));
        QDir().mkpath(dir);
        NativeHost::Options options;
        options.discoveryDirectory = dir;
        const QByteArray status = "{\"op\":\"status\"}";

        options.taskNestRunning = []() { return false; };
        QCOMPARE(codeOf(NativeHost::handle(status, options)), QStringLiteral("not_running"));
        options.taskNestRunning = []() { return true; };
        QCOMPARE(codeOf(NativeHost::handle(status, options)), QStringLiteral("integration_disabled"));

        auto writeDiscovery = [&dir](const QByteArray &content) {
            QFile f(dir + "/integration-0000000000000000.json");
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(content);
        };
        const QString goodToken(64, QLatin1Char('a'));
        const QStringList badFiles = {
            "garbage",
            "[1,2]",
            QStringLiteral("{\"protocol\":1,\"pipe\":\"..\\\\..\\\\evil\",\"token\":\"%1\"}").arg(goodToken),
            QStringLiteral("{\"protocol\":1,\"pipe\":\"\\\\\\\\.\\\\pipe\\\\TaskNest-0123456789abcdef\",\"token\":\"%1\"}").arg(goodToken),
            QStringLiteral("{\"protocol\":1,\"pipe\":\"TaskNest-0123456789ABCDEF\",\"token\":\"%1\"}").arg(goodToken),
            QStringLiteral("{\"protocol\":1,\"pipe\":\"TaskNest-0123456789abcdef\",\"token\":\"short\"}"),
            QStringLiteral("{\"protocol\":2,\"pipe\":\"TaskNest-0123456789abcdef\",\"token\":\"%1\"}").arg(goodToken),
            QStringLiteral("{\"protocol\":1,\"pipe\":\"TaskNest-0123456789abcdef\",\"token\":\"%1\"}")
                .arg(QString(5000, QLatin1Char('a'))),
        };
        for (const QString &content : badFiles) {
            writeDiscovery(content.toUtf8());
            options.taskNestRunning = []() { return false; };
            // an unusable record is "no endpoint", never a crash and never a connection
            QCOMPARE(codeOf(NativeHost::handle(status, options)), QStringLiteral("not_running"));
        }
        // a well-formed record whose pipe nobody serves: unavailable, not a hang
        writeDiscovery(QStringLiteral("{\"protocol\":1,\"pipe\":\"TaskNest-0123456789abcdef\",\"token\":\"%1\"}")
                           .arg(goodToken).toUtf8());
        options.timeoutMs = 500;
        options.taskNestRunning = []() { return true; };
        QCOMPARE(codeOf(NativeHost::handle(status, options)), QStringLiteral("integration_disabled"));

        QVERIFY(IntegrationClient::isValidPipeName("TaskNest-0123456789abcdef"));
        QVERIFY(!IntegrationClient::isValidPipeName("TaskNest-0123456789abcde"));
        QVERIFY(!IntegrationClient::isValidPipeName("\\\\.\\pipe\\TaskNest-0123456789abcdef"));
        QVERIFY(!IntegrationClient::isValidToken(QString(63, QLatin1Char('a'))));
        QVERIFY(IntegrationClient::isValidToken(QString(64, QLatin1Char('f'))));
    }

    void nativehost_capturesThroughTheRealEndpoint()
    {
        IntegrationFixture f(path(QStringLiteral("nh_e2e.db")), path(QStringLiteral("nh_e2e_disc")));
        f.server.setEnabled(true);
        NativeHost::Options options;
        options.discoveryDirectory = f.discovery;
        options.timeoutMs = 4000;
        options.taskNestRunning = []() { return true; };

        auto call = [&options](const QJsonObject &message) {
            const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
            auto future = std::async(std::launch::async, [payload, options]() {
                return NativeHost::handle(payload, options);
            });
            while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
                QCoreApplication::processEvents();
            return future.get();
        };

        const QJsonObject status = call({{"op", "status"}});
        QVERIFY(status.value("ok").toBool());
        QCOMPARE(status.value("result").toObject().value("state").toString(), QStringLiteral("ready"));

        const int before = taskCount(f.repo);
        const QString text = QStringLiteral("Line one \u00E9\u4E2D\u6587 \U0001F600\n<b>not bold</b> & <script>alert(1)</script>\n\tTabbed");
        QJsonObject capture{{"op", "capture"}, {"title", "  A  captured\n title "},
                            {"url", "https://example.com/article?id=7#part"},
                            {"text", text}, {"request_id", "browser-req-0001"}};
        const QJsonObject r = call(capture);
        QVERIFY2(r.value("ok").toBool(), qPrintable(codeOf(r)));
        QCOMPARE(taskCount(f.repo), before + 1);
        const qint64 id = static_cast<qint64>(r.value("result").toObject().value("task_id").toDouble());
        Task stored;
        QVERIFY(f.repo.getTask(id, &stored));
        QCOMPARE(stored.title, QStringLiteral("A captured title"));
        QCOMPARE(stored.notes, text + "\n\nSource: https://example.com/article?id=7#part");
        QVERIFY(!stored.completed);

        // the same request again (a retry) is the same task
        const QJsonObject again = call(capture);
        QVERIFY(again.value("ok").toBool());
        QVERIFY(again.value("result").toObject().value("duplicate").toBool());
        QCOMPARE(taskCount(f.repo), before + 1);

        // an untitled capture takes its title from the first line of the text
        const QJsonObject untitled = call({{"op", "capture"}, {"url", "https://example.com/"},
                                           {"text", "\n  First real line  \nsecond"}});
        QVERIFY(untitled.value("ok").toBool());
        Task t2;
        QVERIFY(f.repo.getTask(static_cast<qint64>(untitled.value("result").toObject().value("task_id").toDouble()), &t2));
        QCOMPARE(t2.title, QStringLiteral("First real line"));

        // addresses the API refuses create nothing
        for (const QString &url : {QStringLiteral("javascript:alert(1)"), QStringLiteral("file:///C:/x"),
                                   QStringLiteral("chrome://settings"), QStringLiteral("ftp://example.com/x"),
                                   QStringLiteral("https://"), QStringLiteral("not a url")}) {
            const QJsonObject bad = call({{"op", "capture"}, {"url", url}, {"text", "some text"}});
            QVERIFY2(!bad.value("ok").toBool(), qPrintable(url));
            QCOMPARE(codeOf(bad), QStringLiteral("invalid_argument"));
        }
        QCOMPARE(taskCount(f.repo), before + 2);

        // the credential never appears in any reply
        const QString token = f.server.token();
        for (const QJsonObject &reply : {status, r, again, untitled})
            QVERIFY(!QJsonDocument(reply).toJson().contains(token.toUtf8()));

        // reminder/cleanup invariants: a captured task is an ordinary task
        QVERIFY(!stored.reminderAt.has_value());
        QVERIFY(f.model.setCompleted(id, true));
        QVERIFY(f.repo.getTask(id, &stored));
        QVERIFY(stored.completed);
    }

    void nativehost_authenticationFailureTimeoutAndShutdown()
    {
        IntegrationFixture f(path(QStringLiteral("nh_auth.db")), path(QStringLiteral("nh_auth_disc")));
        f.server.setEnabled(true);
        NativeHost::Options options;
        options.discoveryDirectory = f.discovery;
        options.timeoutMs = 4000;
        options.taskNestRunning = []() { return true; };
        auto call = [](const QJsonObject &message, NativeHost::Options o) {
            const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
            auto future = std::async(std::launch::async, [payload, o]() {
                return NativeHost::handle(payload, o);
            });
            while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
                QCoreApplication::processEvents();
            return future.get();
        };
        const QJsonObject capture{{"op", "capture"}, {"url", "https://example.com/"}, {"text", "hello"}};
        const int before = taskCount(f.repo);

        // authentication failure: the file's token is not the server's
        const QString file = f.server.discoveryFilePath();
        QFile discovery(file);
        QVERIFY(discovery.open(QIODevice::ReadOnly));
        QJsonObject record = QJsonDocument::fromJson(discovery.readAll()).object();
        discovery.close();
        const QString realToken = record.value("token").toString();
        record.insert("token", QString(64, QLatin1Char('0')));
        QVERIFY(discovery.open(QIODevice::WriteOnly | QIODevice::Truncate));
        discovery.write(QJsonDocument(record).toJson());
        discovery.close();
        const QJsonObject denied = call(capture, options);
        QCOMPARE(codeOf(denied), QStringLiteral("unauthorized"));
        QVERIFY(!QJsonDocument(denied).toJson().contains(realToken.toUtf8()));
        QCOMPARE(taskCount(f.repo), before);

        // timeout: an endpoint that accepts and never answers
        QLocalServer silent;
        QVERIFY(silent.listen(QStringLiteral("TaskNest-00000000deadbeef")));
        record.insert("pipe", "TaskNest-00000000deadbeef");
        QVERIFY(discovery.open(QIODevice::WriteOnly | QIODevice::Truncate));
        discovery.write(QJsonDocument(record).toJson());
        discovery.close();
        NativeHost::Options quick = options;
        quick.timeoutMs = 400;
        // a capture that was sent and never answered may have been stored: unknown, not "timeout"
        QCOMPARE(codeOf(call(capture, quick)), QStringLiteral("outcome_unknown"));
        // a status check changes nothing, so a missing answer is simply a timeout
        QCOMPARE(codeOf(call({{"op", "status"}}, quick)), QStringLiteral("timeout"));
        silent.close();

        // disconnect: the endpoint is gone after the file was written
        QCOMPARE(codeOf(call(capture, quick)), QStringLiteral("integration_disabled"));
        options.taskNestRunning = []() { return false; };
        quick.taskNestRunning = options.taskNestRunning;
        QCOMPARE(codeOf(call(capture, quick)), QStringLiteral("not_running"));

        // a real shutdown of the endpoint removes the file: not running
        f.server.setEnabled(false);
        QVERIFY(!QFileInfo::exists(file));
        QCOMPARE(codeOf(call(capture, options)), QStringLiteral("not_running"));
        QCOMPARE(taskCount(f.repo), before);
    }

    // ---- capture timeouts: confirmed failure vs unknown outcome ----------------
    // A FaultProxy sits in front of the real server, so every assertion about
    // "was it stored" is read from the database, not inferred from the reply.

    void capturetimeout_slowButSuccessfulCaptureIsConfirmed()
    {
        ProxyRig rig(path("ct_slow.db"), path("ct_slow_real"), path("ct_slow_proxy"),
                     QStringLiteral("TaskNest-00000000c0de0001"));
        QVERIFY(rig.proxy->listening());
        rig.proxy->mode = FaultProxy::Mode::SlowReply;
        rig.proxy->delayMs = 900; // slower than any normal answer, well inside the limit
        const QJsonObject r = hostCall(captureMessage("slow-req-0001"), rig.options);
        QVERIFY2(r.value("ok").toBool(), qPrintable(codeOf(r)));
        const QList<qint64> ids = taskIds(rig.f.repo);
        QCOMPARE(ids.size(), 1);
        QCOMPARE(static_cast<qint64>(r.value("result").toObject().value("task_id").toDouble()), ids.first());
        QVERIFY(!r.value("result").toObject().value("duplicate").toBool());
        QVERIFY(r.value("timing_ms").toObject().value("answer").toDouble() >= 800);
        QVERIFY(!QJsonDocument(r).toJson().contains(rig.f.server.token().toUtf8()));
    }

    void capturetimeout_failureBeforeTheRequestIsSentIsConfirmedAndStoresNothing()
    {
        // nothing is listening on the pipe the discovery file names
        ProxyRig rig(path("ct_pre.db"), path("ct_pre_real"), path("ct_pre_proxy"),
                     QStringLiteral("TaskNest-00000000c0de0002"));
        rig.proxy.reset();
        rig.options.timeoutMs = 400;
        const QJsonObject r = hostCall(captureMessage("pre-req-00001"), rig.options);
        QVERIFY(!r.value("ok").toBool());
        QCOMPARE(codeOf(r), QStringLiteral("integration_disabled")); // a definite "not stored"
        QCOMPARE(taskIds(rig.f.repo).size(), 0);
    }

    void capturetimeout_timeoutBeforeTheServerSeesTheRequestIsUnknownAndRetryStoresOnce()
    {
        ProxyRig rig(path("ct_swallow.db"), path("ct_swallow_real"), path("ct_swallow_proxy"),
                     QStringLiteral("TaskNest-00000000c0de0003"));
        QVERIFY(rig.proxy->listening());
        rig.proxy->mode = FaultProxy::Mode::SwallowRequest;
        rig.options.timeoutMs = 400;
        const QJsonObject first = hostCall(captureMessage("swallow-req-01"), rig.options);
        // the host cannot know the request never arrived: it must not say "failed"
        QCOMPARE(codeOf(first), QStringLiteral("outcome_unknown"));
        QCOMPARE(rig.proxy->forwarded, 0);
        QCOMPARE(taskIds(rig.f.repo).size(), 0);

        rig.proxy->mode = FaultProxy::Mode::Relay;
        rig.options.timeoutMs = 3000;
        const QJsonObject retry = hostCall(captureMessage("swallow-req-01"), rig.options);
        QVERIFY2(retry.value("ok").toBool(), qPrintable(codeOf(retry)));
        QVERIFY(!retry.value("result").toObject().value("duplicate").toBool()); // really created now
        const QList<qint64> ids = taskIds(rig.f.repo);
        QCOMPARE(ids.size(), 1);
        QCOMPARE(static_cast<qint64>(retry.value("result").toObject().value("task_id").toDouble()), ids.first());
    }

    void capturetimeout_timeoutAfterAcceptanceThenSameIdRetryReturnsTheOriginalTask()
    {
        ProxyRig rig(path("ct_late.db"), path("ct_late_real"), path("ct_late_proxy"),
                     QStringLiteral("TaskNest-00000000c0de0004"));
        QVERIFY(rig.proxy->listening());
        rig.proxy->mode = FaultProxy::Mode::SlowReply;
        rig.proxy->delayMs = 1500;
        rig.options.timeoutMs = 400;
        const QJsonObject first = hostCall(captureMessage("late-req-00001"), rig.options);
        QCOMPARE(codeOf(first), QStringLiteral("outcome_unknown"));
        // the server did accept it and the task exists although the answer was missed
        QTRY_COMPARE_WITH_TIMEOUT(taskIds(rig.f.repo).size(), 1, 3000);
        const qint64 original = taskIds(rig.f.repo).first();

        rig.proxy->mode = FaultProxy::Mode::Relay;
        rig.options.timeoutMs = 3000;
        const QJsonObject retry = hostCall(captureMessage("late-req-00001"), rig.options);
        QVERIFY2(retry.value("ok").toBool(), qPrintable(codeOf(retry)));
        QVERIFY(retry.value("result").toObject().value("duplicate").toBool());
        QCOMPARE(static_cast<qint64>(retry.value("result").toObject().value("task_id").toDouble()), original);
        QCOMPARE(taskIds(rig.f.repo).size(), 1); // still exactly one task
    }

    void capturetimeout_newRequestIdForTheSamePageCreatesASeparateTask()
    {
        ProxyRig rig(path("ct_new.db"), path("ct_new_real"), path("ct_new_proxy"),
                     QStringLiteral("TaskNest-00000000c0de0005"));
        QVERIFY(rig.proxy->listening());
        const QJsonObject a = hostCall(captureMessage("new-req-000001"), rig.options);
        const QJsonObject b = hostCall(captureMessage("new-req-000002"), rig.options); // same page, new capture
        QVERIFY(a.value("ok").toBool() && b.value("ok").toBool());
        QVERIFY(!b.value("result").toObject().value("duplicate").toBool());
        const qint64 idA = static_cast<qint64>(a.value("result").toObject().value("task_id").toDouble());
        const qint64 idB = static_cast<qint64>(b.value("result").toObject().value("task_id").toDouble());
        QVERIFY(idA != idB);
        QCOMPARE(taskIds(rig.f.repo), (QList<qint64>{qMin(idA, idB), qMax(idA, idB)}));
    }

    void capturetimeout_disconnectWhileTheAnswerIsDeliveredIsUnknownAndRetrySafe()
    {
        ProxyRig rig(path("ct_close.db"), path("ct_close_real"), path("ct_close_proxy"),
                     QStringLiteral("TaskNest-00000000c0de0006"));
        QVERIFY(rig.proxy->listening());
        rig.proxy->mode = FaultProxy::Mode::CloseOnReply;
        const QJsonObject first = hostCall(captureMessage("close-req-00001"), rig.options);
        QCOMPARE(codeOf(first), QStringLiteral("outcome_unknown"));
        QTRY_COMPARE_WITH_TIMEOUT(taskIds(rig.f.repo).size(), 1, 3000);
        const qint64 original = taskIds(rig.f.repo).first();

        rig.proxy->mode = FaultProxy::Mode::Relay;
        const QJsonObject retry = hostCall(captureMessage("close-req-00001"), rig.options);
        QVERIFY(retry.value("result").toObject().value("duplicate").toBool());
        QCOMPARE(static_cast<qint64>(retry.value("result").toObject().value("task_id").toDouble()), original);
        QCOMPARE(taskIds(rig.f.repo).size(), 1);
    }

    void capturetimeout_serverKeepsAnAnsweredConnectionOpenUntilTheClientReads()
    {
        // regression: the server used to close the pipe right after writing, which
        // could discard an answer the client had not read yet (task stored, answer
        // lost, reported as a timeout after only milliseconds).
        // Deterministic: the point where "the server has answered" is observed from
        // the database (the task is stored in the same handler that writes the
        // reply), and the client is held back by a gate, not by a sleep.
        IntegrationFixture f(path("ct_linger.db"), path("ct_linger_disc"));
        f.server.setEnabled(true);
        DiscoveryRecord record;
        QVERIFY(IntegrationClient::readDiscovery(f.server.discoveryFilePath(), &record));
        auto request = [&record](int i) {
            QJsonObject args{{"url", "https://example.com/late"}, {"title", "late"},
                             {"request_id", QStringLiteral("linger-req-%1").arg(i, 4, 10, QLatin1Char('0'))}};
            QJsonObject req{{"v", 1}, {"token", record.token}, {"op", "capture_page"}, {"args", args}};
            return QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
        };

        // Part 1: a client that has sent its request and is not reading yet (its
        // thread is parked on the gate and runs no event loop, like a descheduled
        // host). The server must still hold the connection once it has answered.
        {
            QLocalSocket client;
            client.connectToServer(record.pipe);
            QTRY_VERIFY2_WITH_TIMEOUT(client.state() == QLocalSocket::ConnectedState, "could not connect to the server", 2000);
            QSignalSpy closed(&client, &QLocalSocket::disconnected);
            client.write(request(0));
            QTRY_COMPARE_WITH_TIMEOUT(taskIds(f.repo).size(), 1, 3000); // the server has handled it and written the answer
            QVERIFY2(closed.isEmpty() && !closed.wait(500),
                     "CONNECTION CLOSED PREMATURELY: the server hung up on a client that had not read its answer");
            QVERIFY2(client.state() == QLocalSocket::ConnectedState, "CONNECTION CLOSED PREMATURELY: client is no longer connected before it read");
            QByteArray data;
            while (!data.contains('\n') && client.waitForReadyRead(1000))
                data += client.readAll();
            data += client.readAll();
            QVERIFY2(!data.isEmpty(), "REPLY LOST: the task was stored but no answer reached the client");
            QVERIFY2(data.contains("\"ok\":true"), data.constData());
        }

        // Part 2: the reader lives on its own thread and only starts reading after
        // the main thread has seen the task stored (gate), never after a fixed delay.
        for (int i = 1; i <= 5; ++i) {
            std::promise<void> gate;
            std::promise<void> sent;
            auto sentFuture = sent.get_future();
            auto lateReader = [&](std::shared_future<void> go) {
                QLocalSocket client;
                client.connectToServer(record.pipe);
                if (!client.waitForConnected(2000)) {
                    sent.set_value();
                    return QByteArray("NO CONNECTION: could not connect to the server");
                }
                client.write(request(i));
                client.waitForBytesWritten(1000);
                sent.set_value();
                go.wait(); // parked: the answer is written while nobody reads it
                QByteArray data;
                while (!data.contains('\n') && client.waitForReadyRead(1000))
                    data += client.readAll();
                data += client.readAll();
                return data;
            };
            auto future = std::async(std::launch::async, lateReader, gate.get_future().share());
            while (sentFuture.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
                QCoreApplication::processEvents();
            QTRY_COMPARE_WITH_TIMEOUT(taskIds(f.repo).size(), i + 1, 3000);
            for (int round = 0; round < 50; ++round) // let the server finish everything it will do
                QCoreApplication::processEvents();
            gate.set_value();
            while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
                QCoreApplication::processEvents();
            const QByteArray data = future.get();
            QVERIFY2(!data.isEmpty(), "REPLY LOST: the task was stored but no answer reached the late reader");
            QVERIFY2(data.contains("\"ok\":true"), data.constData());
        }
        QCOMPARE(taskIds(f.repo).size(), 6);
    }

    void capturetimeout_concurrentRepeatsOfOneRequestIdStoreOneTask()
    {
        ProxyRig rig(path("ct_conc.db"), path("ct_conc_real"), path("ct_conc_proxy"),
                     QStringLiteral("TaskNest-00000000c0de0007"));
        QVERIFY(rig.proxy->listening());
        const QByteArray payload = QJsonDocument(captureMessage("conc-req-00001")).toJson(QJsonDocument::Compact);
        const NativeHost::Options options = rig.options;
        auto one = [payload, options]() { return NativeHost::handle(payload, options); };
        auto f1 = std::async(std::launch::async, one);
        auto f2 = std::async(std::launch::async, one);
        auto f3 = std::async(std::launch::async, one);
        for (auto *f : {&f1, &f2, &f3})
            while (f->wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
                QCoreApplication::processEvents();
        const QJsonObject r1 = f1.get(), r2 = f2.get(), r3 = f3.get();
        const QList<qint64> ids = taskIds(rig.f.repo);
        QCOMPARE(ids.size(), 1);
        for (const QJsonObject &r : {r1, r2, r3}) {
            QVERIFY2(r.value("ok").toBool(), qPrintable(codeOf(r)));
            QCOMPARE(static_cast<qint64>(r.value("result").toObject().value("task_id").toDouble()), ids.first());
        }
    }

#ifdef Q_OS_WIN
    void capturetimeout_hostProcessAnswersAfterATimeoutAndExits()
    {
        // the real TaskNestHost.exe, as a browser starts it: stdin stays open while
        // it waits for the reply. After the timeout it must answer once with an
        // unknown outcome and be gone, without any help from the browser.
        const TestProfile::Profile profile = TestProfile::fromRoot(path(QStringLiteral("ct_proc_profile")), true);
        QVERIFY2(profile.active, qPrintable(profile.error));
        // the host reads only the profile's own discovery folder, so the proxy's
        // discovery file is written there
        ProxyRig rig(profile.databasePath, path("ct_proc_real"), profile.discoveryDirectory,
                     QStringLiteral("TaskNest-00000000c0de0008"));
        QVERIFY(rig.proxy->listening());
        rig.proxy->mode = FaultProxy::Mode::DropReply; // request reaches the server, answer never returns

        auto runHost = [&](const QString &requestId, QJsonObject *reply, qint64 *pid) {
            QProcess host;
            host.setProgram(QStringLiteral(TASKNEST_HOST_EXE));
            host.setArguments({QStringLiteral("--test-profile"), profile.root});
            host.start();
            if (!host.waitForStarted(5000))
                return false;
            *pid = host.processId();
            const QByteArray body = QJsonDocument(captureMessage(requestId)).toJson(QJsonDocument::Compact);
            host.write(NativeHost::encodeFrame(body)); // stdin is left open
            QElapsedTimer clock;
            clock.start();
            while (host.state() != QProcess::NotRunning && clock.elapsed() < 9000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                host.waitForFinished(5);
            }
            const bool exited = host.state() == QProcess::NotRunning;
            if (!exited) {
                host.kill();
                host.waitForFinished(3000);
            }
            const QByteArray out = host.readAllStandardOutput();
            if (out.size() > 4)
                *reply = QJsonDocument::fromJson(out.mid(4)).object();
            return exited && host.exitCode() == 0 && clock.elapsed() < 8000;
        };
        QJsonObject first;
        qint64 pid = 0;
        QVERIFY2(runHost(QStringLiteral("proc-req-00001"), &first, &pid), "host did not exit by itself");
        QCOMPARE(codeOf(first), QStringLiteral("outcome_unknown"));
        QVERIFY(pid > 0);
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (h) { // if the pid can still be opened, the process must be finished
            DWORD code = STILL_ACTIVE;
            GetExitCodeProcess(h, &code);
            CloseHandle(h);
            QVERIFY(code != STILL_ACTIVE);
        }
        QTRY_COMPARE_WITH_TIMEOUT(taskIds(rig.f.repo).size(), 1, 3000); // stored although unanswered
        const qint64 original = taskIds(rig.f.repo).first();

        rig.proxy->mode = FaultProxy::Mode::Relay;
        QJsonObject retry;
        QVERIFY(runHost(QStringLiteral("proc-req-00001"), &retry, &pid));
        QVERIFY2(retry.value("ok").toBool(), qPrintable(codeOf(retry)));
        QVERIFY(retry.value("result").toObject().value("duplicate").toBool());
        QCOMPARE(static_cast<qint64>(retry.value("result").toObject().value("task_id").toDouble()), original);
        QCOMPARE(taskIds(rig.f.repo).size(), 1);
    }
#endif

#ifdef Q_OS_WIN
    void nativehost_registrationIsScopedToTheExtensionAndCleansUp()
    {
        const QString base = QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTests\\NM%1")
                                 .arg(QCoreApplication::applicationPid());
        const QString manifestDir = path(QStringLiteral("nm_manifest"));
        const QString hostExe = path(QStringLiteral("TaskNestHost.exe"));
        QFile fake(hostExe);
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("MZ");
        fake.close();

        {
            NativeHostRegistration reg(base, manifestDir);
            QVERIFY(!reg.isRegistered(hostExe));
            QVERIFY(!RegistryUtil::keyExists(reg.registryKeys().first())); // asking creates nothing
            QString why;
            QVERIFY2(reg.registerHost(hostExe, &why), qPrintable(why));
            QVERIFY(reg.isRegistered(hostExe));

            QFile f(reg.manifestPath());
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QJsonObject m = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            QCOMPARE(m.value("name").toString(), QStringLiteral("com.tasknest.host"));
            QCOMPARE(m.value("type").toString(), QStringLiteral("stdio"));
            const QJsonArray origins = m.value("allowed_origins").toArray();
            QCOMPARE(origins.size(), 1);   // exactly one, never a wildcard
            QCOMPARE(origins.first().toString(),
                     QStringLiteral("chrome-extension://%1/").arg(NativeHost::kExtensionId));
            QVERIFY(!origins.first().toString().contains('*'));
            QCOMPARE(QDir::fromNativeSeparators(m.value("path").toString()),
                     QDir::fromNativeSeparators(hostExe));

            // both browsers point at the same manifest
            const QStringList keys = reg.registryKeys();
            QCOMPARE(keys.size(), 2);
            QVERIFY(keys.at(0).contains("Google\\Chrome\\NativeMessagingHosts"));
            QVERIFY(keys.at(1).contains("Microsoft\\Edge\\NativeMessagingHosts"));
            for (const QString &key : keys) {
                QSettings s(key, QSettings::NativeFormat);
                QCOMPARE(QDir::fromNativeSeparators(s.value("Default").toString()),
                         QDir::fromNativeSeparators(reg.manifestPath()));
            }
            QVERIFY(reg.registerHost(hostExe)); // repeating is harmless
            QVERIFY(reg.isRegistered(hostExe));

            QVERIFY(reg.unregisterHost());
            QVERIFY(!reg.isRegistered(hostExe));
            QVERIFY(!QFileInfo::exists(reg.manifestPath()));
            QVERIFY(QFileInfo::exists(hostExe)); // the host program itself is not touched
            QVERIFY(reg.unregisterHost());       // removing what is not there is fine

            // a missing host program is refused with a reason
            NativeHostRegistration reg2(base, manifestDir);
            QVERIFY(!reg2.registerHost(path(QStringLiteral("missing/TaskNestHost.exe")), &why));
            QVERIFY(!why.isEmpty());
        }
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTests"),
                  QSettings::NativeFormat).remove(QString());
    }
#endif

    void extension_manifestMatchesTheHostRegistrationAndStaysMinimal()
    {
        QFile f(QStringLiteral(TASKNEST_SOURCE_DIR "/browser-extension/manifest.json"));
        QVERIFY2(f.open(QIODevice::ReadOnly), "browser-extension/manifest.json");
        const QJsonObject m = QJsonDocument::fromJson(f.readAll()).object();

        QCOMPARE(m.value("manifest_version").toInt(), 3);
        // the stable id the host registration is limited to comes from the key
        const QByteArray der = QByteArray::fromBase64(m.value("key").toString().toLatin1());
        QVERIFY(der.size() > 100);
        const QByteArray hash = QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex().left(32);
        QString id;
        for (const char c : hash)
            id += QChar('a' + (c >= 'a' ? c - 'a' + 10 : c - '0'));
        QCOMPARE(id, QString::fromLatin1(NativeHost::kExtensionId));

        // only the permissions the code needs; no site access, no injected scripts
        QStringList permissions;
        for (const QJsonValue &p : m.value("permissions").toArray())
            permissions << p.toString();
        permissions.sort();
        QCOMPARE(permissions, (QStringList{"activeTab", "contextMenus", "nativeMessaging", "scripting"}));
        QVERIFY(!m.contains("host_permissions"));
        QVERIFY(!m.contains("optional_host_permissions"));
        QVERIFY(!m.contains("content_scripts"));
        QVERIFY(!m.contains("externally_connectable"));
        QVERIFY(!m.contains("web_accessible_resources"));
        QVERIFY(!m.contains("content_security_policy")); // the strict MV3 default applies
    }

    void extension_sourceHasNoDynamicHtmlOrLogging()
    {
        const QStringList files = {"popup.js", "background.js", "capture-core.js", "popup.html"};
        for (const QString &name : files) {
            QFile f(QStringLiteral(TASKNEST_SOURCE_DIR "/browser-extension/") + name);
            QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(name));
            const QString src = QString::fromUtf8(f.readAll());
            QVERIFY2(!src.contains("innerHTML"), qPrintable(name));
            QVERIFY2(!src.contains("outerHTML"), qPrintable(name));
            QVERIFY2(!src.contains("insertAdjacentHTML"), qPrintable(name));
            QVERIFY2(!src.contains("document.write"), qPrintable(name));
            QVERIFY2(!src.contains("eval("), qPrintable(name));
            QVERIFY2(!src.contains("console."), qPrintable(name));
            QVERIFY2(!src.contains("fetch("), qPrintable(name));
            QVERIFY2(!src.contains("XMLHttpRequest"), qPrintable(name));
            QVERIFY2(!src.contains("localStorage"), qPrintable(name));
            QVERIFY2(!src.contains("token"), qPrintable(name)); // the extension never handles one
        }
    }

    // ---- capture from other applications --------------------------------------

    void selection_textIsSanitizedAndSourceIsFormatted()
    {
        QCOMPARE(SelectionCapture::sanitize(QStringLiteral("  a\r\nb\rc\x01\x7f\td  ")),
                 QStringLiteral("a\nb\nc\td"));
        QCOMPARE(SelectionCapture::sanitize(QStringLiteral("\u00E9\u4E2D\U0001F600")),
                 QStringLiteral("\u00E9\u4E2D\U0001F600"));
        QCOMPARE(SelectionCapture::sanitize(QStringLiteral(" \n\t ")), QString());

        SelectionResult r;
        r.sourceTitle = QStringLiteral("  Report \n final.pdf - Reader ");
        r.sourceProcess = QStringLiteral("reader.exe");
        QCOMPARE(r.sourceText(), QStringLiteral("Report final.pdf - Reader (reader.exe)"));
        r.sourceTitle.clear();
        QCOMPARE(r.sourceText(), QStringLiteral("reader.exe"));
        r.sourceProcess.clear();
        QVERIFY(r.sourceText().isEmpty());

        r.status = SelectionResult::Password;
        r.message = QStringLiteral("A password field is focused; nothing was read.");
        const QVariantMap m = r.toVariantMap();
        QCOMPARE(m.value("status").toString(), QStringLiteral("password"));
        QVERIFY(m.value("text").toString().isEmpty());
    }

    // ---- hardening ---------------------------------------------------------------

    void integration_requestIdStoreIsBoundedAndOnlyRepeatsAreMerged()
    {
        IntegrationFixture f(path(QStringLiteral("int_rid_bound.db")), path(QStringLiteral("int_rid_bound_disc")));
        f.server.setEnabled(true);
        const QString tok = f.server.token();
        auto capture = [&](const QString &id) {
            QJsonObject a;
            a.insert("url", "https://example.com/p");
            a.insert("request_id", id);
            return f.server.handle(request(tok, "capture_page", a));
        };
        const int before = taskCount(f.repo);
        const int count = IntegrationServer::kRememberedRequestIds + 22;
        for (int i = 0; i < count; ++i)
            QVERIFY(capture(QStringLiteral("bounded-%1-abcdef").arg(i)).value("ok").toBool());
        QCOMPARE(taskCount(f.repo), before + count);

        // the newest ids are still remembered; the oldest were forgotten (bounded
        // memory), so an intentional repeat of an old id is just a new task
        const QJsonObject recent = capture(QStringLiteral("bounded-%1-abcdef").arg(count - 1));
        QVERIFY(recent.value("result").toObject().value("duplicate").toBool());
        QCOMPARE(taskCount(f.repo), before + count);
        const QJsonObject old = capture(QStringLiteral("bounded-0-abcdef"));
        QVERIFY(old.value("ok").toBool());
        QVERIFY(!old.value("result").toObject().contains("duplicate"));
        QCOMPARE(taskCount(f.repo), before + count + 1);

        // refused requests leave nothing behind: the same id works once the
        // request is valid
        QJsonObject bad;
        bad.insert("url", "javascript:alert(1)");
        bad.insert("request_id", "not-stored-0001");
        QCOMPARE(codeOf(f.server.handle(request(tok, "capture_page", bad))), QStringLiteral("invalid_argument"));
        QJsonObject good;
        good.insert("url", "https://example.com/ok");
        good.insert("request_id", "not-stored-0001");
        const QJsonObject r = f.server.handle(request(tok, "capture_page", good));
        QVERIFY(r.value("ok").toBool());
        QVERIFY(!r.value("result").toObject().contains("duplicate"));
    }

#ifdef Q_OS_WIN
    void nativehost_launcherSuppliesTheQtRuntimeWithoutBundlingAnything()
    {
        const QString base = QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTests\\NMW%1")
                                 .arg(QCoreApplication::applicationPid());
        const QString manifestDir = path(QStringLiteral("nmw_manifest"));
        const QString hostDir = path(QStringLiteral("nmw host dir"));
        const QString runtimeDir = path(QStringLiteral("nmw runtime"));
        QDir().mkpath(hostDir);
        QDir().mkpath(runtimeDir);
        const QString hostExe = hostDir + "/TaskNestHost.exe";
        QFile fake(hostExe);
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("MZ");
        fake.close();

        // this process's own Qt runtime folders are found, are real folders and
        // are safe to place in a script
        const QStringList dirs = NativeHostRegistration::runtimeDirectories(hostExe);
        QVERIFY(!dirs.isEmpty());
        for (const QString &d : dirs) {
            QVERIFY(QFileInfo(d).isDir());
            QVERIFY2(NativeHostRegistration::isSafeDirectory(d), qPrintable(d));
        }

        NativeHostRegistration reg(base, manifestDir);
        QString why;
        QVERIFY2(reg.registerHost(hostExe, &why, {runtimeDir}), qPrintable(why));
        QVERIFY(reg.isRegistered(hostExe));
        QFile manifestFile(reg.manifestPath());
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const QJsonObject m = QJsonDocument::fromJson(manifestFile.readAll()).object();
        manifestFile.close();
        // the browser starts the launcher; the launcher starts the host
        QCOMPARE(QDir::fromNativeSeparators(m.value("path").toString()),
                 QDir::fromNativeSeparators(reg.launcherPath()));
        QCOMPARE(m.value("allowed_origins").toArray().size(), 1);
        QFile launcher(reg.launcherPath());
        QVERIFY(launcher.open(QIODevice::ReadOnly));
        const QString script = QString::fromLocal8Bit(launcher.readAll());
        launcher.close();
        QVERIFY(script.contains(QStringLiteral("set \"PATH=%1;%PATH%\"").arg(QDir::toNativeSeparators(runtimeDir))));
        QVERIFY(script.contains(QStringLiteral("\"%1\" %*").arg(QDir::toNativeSeparators(hostExe))));
        QCOMPARE(script.count(QLatin1String("TaskNestHost.exe")), 1); // one program only

        // names a command interpreter would treat specially are refused, and
        // the previous registration stays intact
        for (const QString &name : {QStringLiteral("a&b"), QStringLiteral("a%PATH%b"), QStringLiteral("a\"b")}) {
            const QString evil = path(name);
            QDir().mkpath(evil);
            QVERIFY(!NativeHostRegistration::isSafeDirectory(evil) || QFileInfo(evil).fileName() == name);
        }
        QVERIFY(!NativeHostRegistration::isSafeDirectory(path(QStringLiteral("does not exist"))));
        QVERIFY(!NativeHostRegistration::isSafeDirectory(QString()));
        QVERIFY(!NativeHostRegistration::isSafeDirectory(QStringLiteral("C:\\a&calc")));
        QVERIFY(!reg.registerHost(hostExe, &why, {QStringLiteral("C:\\Windows&calc")}));
        QVERIFY(reg.isRegistered(hostExe));

        // registering again without extra folders goes back to the direct path
        QVERIFY(reg.registerHost(hostExe, &why));
        QVERIFY(!QFileInfo::exists(reg.launcherPath()));
        QVERIFY(reg.isRegistered(hostExe));

        QVERIFY(reg.registerHost(hostExe, &why, {runtimeDir}));
        QVERIFY(reg.unregisterHost());
        QVERIFY(!QFileInfo::exists(reg.launcherPath()));
        QVERIFY(!QFileInfo::exists(reg.manifestPath()));
        QVERIFY(QFileInfo::exists(hostExe));
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTests"),
                  QSettings::NativeFormat).remove(QString());
    }

    void nativehost_unregisterLeavesUnrelatedEntriesAlone()
    {
        const QString base = QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTests\\NMU%1")
                                 .arg(QCoreApplication::applicationPid());
        const QString manifestDir = path(QStringLiteral("nmu_manifest"));
        const QString hostExe = path(QStringLiteral("TaskNestHost.exe"));
        QFile fake(hostExe);
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("MZ");
        fake.close();
        // another application's host registered in the same two browser trees
        const QStringList otherKeys = {
            base + "\\Google\\Chrome\\NativeMessagingHosts\\com.example.other",
            base + "\\Microsoft\\Edge\\NativeMessagingHosts\\com.example.other"};
        for (const QString &k : otherKeys) {
            QSettings s(k, QSettings::NativeFormat);
            s.setValue("Default", "C:\\Other\\host.json");
            s.sync();
        }
        QFile otherManifest(manifestDir + "/other.json");
        QDir().mkpath(manifestDir);
        QVERIFY(otherManifest.open(QIODevice::WriteOnly));
        otherManifest.write("{}");
        otherManifest.close();

        NativeHostRegistration reg(base, manifestDir);
        QVERIFY(reg.registerHost(hostExe));
        QVERIFY(reg.unregisterHost());
        for (const QString &k : otherKeys) {
            QSettings s(k, QSettings::NativeFormat);
            QCOMPARE(s.value("Default").toString(), QStringLiteral("C:\\Other\\host.json"));
        }
        QVERIFY(QFileInfo::exists(manifestDir + "/other.json"));
        QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTests"),
                  QSettings::NativeFormat).remove(QString());
    }
#endif

    void security_noHooksKeystrokesClipboardOrContentLogging()
    {
        const QString root = QStringLiteral(TASKNEST_SOURCE_DIR "/scripts");
        QStringList files;
        for (const QString &sub : {QStringLiteral("src"), QStringLiteral("include"), QStringLiteral("main")}) {
            const QDir dir(root + "/" + sub);
            for (const QFileInfo &fi : dir.entryInfoList({"*.cpp", "*.h"}, QDir::Files))
                files << fi.absoluteFilePath();
        }
        QVERIFY(files.size() > 10);
        const QStringList forbidden = {"SetWindowsHookEx", "SendInput", "keybd_event", "mouse_event",
                                       "OpenClipboard", "GetClipboardData", "SetClipboardData",
                                       "QClipboard", "QGuiApplication::clipboard", "AddClipboardFormatListener",
                                       "SetClipboardViewer", "RegisterRawInputDevices", "GetAsyncKeyState",
                                       "GetKeyboardState", "CreateService", "StartService"};
        for (const QString &path : files) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString src = QString::fromUtf8(f.readAll());
            for (const QString &word : forbidden)
                QVERIFY2(!src.contains(word), qPrintable(QFileInfo(path).fileName() + " uses " + word));
        }
        // the bridge and the client never log: nothing they handle can leak
        for (const QString &name : {QStringLiteral("src/nativehost.cpp"), QStringLiteral("src/integrationclient.cpp"),
                                    QStringLiteral("main/hostmain.cpp"), QStringLiteral("src/selectioncapture.cpp"),
                                    QStringLiteral("src/integrationserver.cpp")}) {
            QFile f(root + "/" + name);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString src = QString::fromUtf8(f.readAll());
            for (const QString &word : {QStringLiteral("qDebug"), QStringLiteral("qWarning"),
                                        QStringLiteral("qInfo"), QStringLiteral("qCritical"),
                                        QStringLiteral("std::cout"), QStringLiteral("std::cerr"),
                                        QStringLiteral("printf"), QStringLiteral("QMessageLogger")})
                QVERIFY2(!src.contains(word), qPrintable(name + " logs via " + word));
        }
        // no process is ever started by the bridge or the capture code
        for (const QString &name : {QStringLiteral("src/nativehost.cpp"), QStringLiteral("src/integrationclient.cpp"),
                                    QStringLiteral("main/hostmain.cpp"), QStringLiteral("src/selectioncapture.cpp")}) {
            QFile f(root + "/" + name);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString src = QString::fromUtf8(f.readAll());
            for (const QString &word : {QStringLiteral("QProcess"), QStringLiteral("CreateProcess"),
                                        QStringLiteral("ShellExecute"), QStringLiteral("system("),
                                        QStringLiteral("WinExec")})
                QVERIFY2(!src.contains(word), qPrintable(name + " starts a process via " + word));
        }
    }

    void widget_reduceMotionDefaultsOffAndPersists()
    {
        const QString file = path(QStringLiteral("reduce_motion.db"));
        auto session = [&file](const std::function<void(WidgetController &)> &body) {
            Database db;
            QVERIFY(db.initialize(file));
            TaskRepository repo(db);
            TaskListModel model(repo);
            WidgetController widget(model, repo);
            QVERIFY(widget.load());
            body(widget);
        };
        session([](WidgetController &w) {
            QVERIFY(!w.reduceMotion()); // default: full motion
            w.setReduceMotion(true);
            QVERIFY(w.flush());
        });
        session([](WidgetController &w) {
            QVERIFY(w.reduceMotion());   // survived the restart
            QVERIFY(!w.widgetVisible()); // independent of widget visibility
            w.setReduceMotion(false);
            QVERIFY(w.flush());
        });
        session([](WidgetController &w) { QVERIFY(!w.reduceMotion()); });
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

    // ---- scratch test profile (--test-profile) --------------------------------

    void testprofile_selectsAnExplicitScratchLocationAndIsolatesTwoProfiles()
    {
        const QString rootA = path(QStringLiteral("tp_a"));
        const QString rootB = path(QStringLiteral("tp_b"));
        const TestProfile::Profile a = TestProfile::fromArguments(
            {QStringLiteral("TaskNest.exe"), QStringLiteral("--test-profile"), rootA}, true);
        const TestProfile::Profile b = TestProfile::fromRoot(rootB, true);
        QVERIFY2(a.active, qPrintable(a.error));
        QVERIFY2(b.active, qPrintable(b.error));

        QCOMPARE(a.databasePath, QDir::cleanPath(rootA) + QStringLiteral("/data/tasknest.db"));
        QCOMPARE(a.discoveryDirectory, QDir::cleanPath(rootA) + QStringLiteral("/integration"));
        QCOMPARE(a.manifestDirectory, QDir::cleanPath(rootA) + QStringLiteral("/native-host"));
        QVERIFY(QFileInfo::exists(QDir(a.root).filePath(QLatin1String(TestProfile::kMarkerFile))));
        QVERIFY(TestProfile::isProfileRegistryBase(a.registryBase));
        QVERIFY(a.registryBase != b.registryBase);
        QVERIFY(a.databasePath != b.databasePath);
        QVERIFY(a.discoveryDirectory != b.discoveryDirectory);

        // nothing points at the real locations
        QVERIFY(a.discoveryDirectory != IntegrationClient::defaultDiscoveryDirectory());
        QVERIFY(!a.registryBase.contains(QStringLiteral("NativeMessagingHosts")));

        // the databases and endpoints of the two profiles are independent
        IntegrationFixture fa(a.databasePath, a.discoveryDirectory);
        IntegrationFixture fb(b.databasePath, b.discoveryDirectory);
        QVERIFY(QFileInfo::exists(a.databasePath));
        QVERIFY(QFileInfo::exists(b.databasePath));
        fa.server.setEnabled(true);
        fb.server.setEnabled(true);
        QVERIFY(fa.server.discoveryFilePath() != fb.server.discoveryFilePath());
        QVERIFY(fa.server.discoveryFilePath().startsWith(a.discoveryDirectory));
        QVERIFY(fb.server.discoveryFilePath().startsWith(b.discoveryDirectory));
        QVERIFY(QFileInfo::exists(fa.server.discoveryFilePath()));
        QVERIFY(QFileInfo::exists(fb.server.discoveryFilePath()));
        QVERIFY(!fa.server.token().isEmpty());
        QVERIFY(fa.server.token() != fb.server.token());
        QVERIFY(fa.model.addTask(QStringLiteral("only in A"), QString(), QString(), 0, QDateTime()));
        QCOMPARE(taskCount(fa.repo), 1);
        QCOMPARE(taskCount(fb.repo), 0);
    }

    void testprofile_defaultLaunchIsUnchangedAndInvalidOptionsFailClosed()
    {
        // no option: not requested, nothing derived, nothing created
        const TestProfile::Profile none = TestProfile::fromArguments(
            {QStringLiteral("TaskNest.exe"), QStringLiteral("tasknest://view/today")}, true);
        QVERIFY(!none.requested);
        QVERIFY(!none.active);
        QVERIFY(none.error.isEmpty());
        QVERIFY(none.databasePath.isEmpty());
        QVERIFY(none.registryBase.isEmpty());

        // the protected list names the real locations
        const QStringList prot = TestProfile::protectedDirectories();
        QVERIFY(prot.contains(QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).filePath("data")),
                              Qt::CaseInsensitive));

        auto refused = [](const QString &root, bool create = true) {
            const TestProfile::Profile p = TestProfile::fromRoot(root, create);
            return p.requested && !p.active && !p.error.isEmpty();
        };
        QVERIFY(refused(QString()));
        QVERIFY(refused(QStringLiteral("relative/profile")));
        QVERIFY(refused(QStringLiteral("C:/")));
        QVERIFY(refused(QStringLiteral("//server/share/profile")));
        QVERIFY(refused(QStringLiteral("C:/scratch/../profile")));
        QVERIFY(refused(QStringLiteral("C:/scratch\nprofile")));
        QVERIFY(refused(QDir(QCoreApplication::applicationDirPath()).filePath("data")));
        QVERIFY(refused(QCoreApplication::applicationDirPath()));
        QVERIFY(refused(QDir(QCoreApplication::applicationDirPath()).filePath("data/sub")));
        const QString appData = qEnvironmentVariable("APPDATA");
        if (!appData.isEmpty()) {
            QVERIFY(refused(QDir(appData).filePath("TaskNest")));
            QVERIFY(refused(QDir(appData).filePath("TaskNest/nested")));
        }
        const QString local = qEnvironmentVariable("LOCALAPPDATA");
        if (!local.isEmpty())
            QVERIFY(refused(QDir(local).filePath("TaskNest/integration")));

        // a folder that is not ours is not adopted; a missing one is not made
        // unless creation was requested
        const QString foreign = path(QStringLiteral("tp_foreign"));
        QVERIFY(QDir().mkpath(foreign));
        QFile stray(QDir(foreign).filePath(QStringLiteral("notes.txt")));
        QVERIFY(stray.open(QIODevice::WriteOnly));
        stray.close();
        QVERIFY(refused(foreign));
        QVERIFY(!QFileInfo::exists(QDir(foreign).filePath(QLatin1String(TestProfile::kMarkerFile))));
        const QString missing = path(QStringLiteral("tp_missing"));
        QVERIFY(refused(missing, /*create=*/false));
        QVERIFY(!QFileInfo::exists(missing));

        // option syntax
        const TestProfile::Profile noValue = TestProfile::fromArguments(
            {QStringLiteral("x"), QStringLiteral("--test-profile")}, true);
        QVERIFY(noValue.requested && !noValue.active);
        const TestProfile::Profile twice = TestProfile::fromArguments(
            {QStringLiteral("x"), QStringLiteral("--test-profile"), path("tp_t1"),
             QStringLiteral("--test-profile"), path("tp_t2")}, true);
        QVERIFY(twice.requested && !twice.active);
        QVERIFY(!QFileInfo::exists(path("tp_t1")));
        QVERIFY(!QFileInfo::exists(path("tp_t2")));
    }

    void testprofile_hostOptionsFailClosedAndNeverFallBack()
    {
        const QString normalDefault = IntegrationClient::defaultDiscoveryDirectory();
        const QString extOrigin = NativeHost::extensionOrigin();

        // production: browser-supplied arguments are ignored, default endpoint
        const NativeHost::Options prod = NativeHost::optionsFromArguments(
            {QStringLiteral("TaskNestHost.exe"), extOrigin, QStringLiteral("--parent-window=12"),
             QStringLiteral("--something-else")});
        QVERIFY(prod.configurationError.isEmpty());
        QVERIFY(prod.discoveryDirectory.isEmpty());

        // a valid profile: the scratch folder is the only endpoint location
        const TestProfile::Profile p = TestProfile::fromRoot(path(QStringLiteral("tp_host")), true);
        QVERIFY2(p.active, qPrintable(p.error));
        const NativeHost::Options ok = NativeHost::optionsFromArguments(
            {QStringLiteral("TaskNestHost.exe"), extOrigin, QStringLiteral("--parent-window=12"),
             QStringLiteral("--test-profile"), p.root});
        QVERIFY2(ok.configurationError.isEmpty(), qPrintable(ok.configurationError));
        QCOMPARE(ok.discoveryDirectory, p.discoveryDirectory);
        QVERIFY(ok.discoveryDirectory != normalDefault);

        // fail closed: unmarked/missing profile, bad value, extra arguments
        const QString unmarked = path(QStringLiteral("tp_unmarked"));
        QVERIFY(QDir().mkpath(unmarked));
        const QList<QStringList> bad = {
            {QStringLiteral("h"), QStringLiteral("--test-profile"), unmarked},
            {QStringLiteral("h"), QStringLiteral("--test-profile"), path("tp_nope")},
            {QStringLiteral("h"), QStringLiteral("--test-profile")},
            {QStringLiteral("h"), QStringLiteral("--test-profile"), QStringLiteral("relative")},
            {QStringLiteral("h"), QStringLiteral("--test-profile"), p.root, QStringLiteral("--exe=C:/evil.exe")},
            {QStringLiteral("h"), QStringLiteral("--test-profile"), p.root, QStringLiteral("C:/other")},
        };
        for (const QStringList &args : bad) {
            const NativeHost::Options o = NativeHost::optionsFromArguments(args);
            QVERIFY2(!o.configurationError.isEmpty(), qPrintable(args.join(' ')));
            QVERIFY(o.discoveryDirectory != normalDefault);
            // every request is answered with the configuration error, nothing is contacted
            QCOMPARE(codeOf(NativeHost::handle("{\"op\":\"status\"}", o)), QStringLiteral("configuration"));
            QCOMPARE(codeOf(NativeHost::handle("{\"op\":\"capture\",\"text\":\"a\",\"url\":\"https://a.b/\"}", o)),
                     QStringLiteral("configuration"));
        }
        QVERIFY(!QFileInfo::exists(path("tp_nope")));
    }

    void testprofile_hostTalksOnlyToItsOwnScratchEndpoint()
    {
        const TestProfile::Profile a = TestProfile::fromRoot(path(QStringLiteral("tp_e2e_a")), true);
        const TestProfile::Profile b = TestProfile::fromRoot(path(QStringLiteral("tp_e2e_b")), true);
        QVERIFY(a.active && b.active);
        IntegrationFixture fa(a.databasePath, a.discoveryDirectory);
        IntegrationFixture fb(b.databasePath, b.discoveryDirectory);
        fa.server.setEnabled(true);
        fb.server.setEnabled(true);

        auto optionsFor = [](const TestProfile::Profile &p) {
            NativeHost::Options o = NativeHost::optionsFromArguments(
                {QStringLiteral("h"), QStringLiteral("--test-profile"), p.root});
            o.timeoutMs = 4000;
            o.taskNestRunning = []() { return true; };
            return o;
        };
        auto call = [](const QJsonObject &message, NativeHost::Options o) {
            const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
            auto future = std::async(std::launch::async, [payload, o]() {
                return NativeHost::handle(payload, o);
            });
            while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
                QCoreApplication::processEvents();
            return future.get();
        };
        const QJsonObject capture{{"op", "capture"}, {"url", "https://example.com/a"},
                                  {"text", "scratch capture"}, {"request_id", "tp-req-1"}};

        const QJsonObject r = call(capture, optionsFor(a));
        QVERIFY2(r.value("ok").toBool(), qPrintable(codeOf(r)));
        QCOMPARE(taskCount(fa.repo), 1);
        QCOMPARE(taskCount(fb.repo), 0);
        QVERIFY(call(capture, optionsFor(b)).value("ok").toBool()); // same id, other profile: own task
        QCOMPARE(taskCount(fb.repo), 1);
        QCOMPARE(taskCount(fa.repo), 1);

        // a profile with integration off answers "unavailable", never another endpoint
        fa.server.setEnabled(false);
        QVERIFY(!QFileInfo::exists(fa.server.discoveryFilePath()));
        QVERIFY(!call({{"op", "status"}}, optionsFor(a)).value("ok").toBool());
        QVERIFY(call({{"op", "status"}}, optionsFor(b)).value("ok").toBool());

        // wrong credential in the scratch discovery file is refused
        QFile discovery(fb.server.discoveryFilePath());
        QVERIFY(discovery.open(QIODevice::ReadOnly));
        QJsonObject record = QJsonDocument::fromJson(discovery.readAll()).object();
        discovery.close();
        record.insert("token", QString(64, QLatin1Char('0')));
        QVERIFY(discovery.open(QIODevice::WriteOnly | QIODevice::Truncate));
        discovery.write(QJsonDocument(record).toJson());
        discovery.close();
        QCOMPARE(codeOf(call(capture, optionsFor(b))), QStringLiteral("unauthorized"));
        QCOMPARE(taskCount(fb.repo), 1);

        // a discovery file naming a pipe that is not TaskNest's is rejected
        record.insert("pipe", QStringLiteral("\\\\.\\pipe\\evil"));
        QVERIFY(discovery.open(QIODevice::WriteOnly | QIODevice::Truncate));
        discovery.write(QJsonDocument(record).toJson());
        discovery.close();
        QVERIFY(!call({{"op", "status"}}, optionsFor(b)).value("ok").toBool());
    }

    void testprofile_createsTheDataFolderSoTheInstanceLockCanBeTaken()
    {
        // regression: the lock sits beside the database and is taken before the
        // database layer creates the folder, so the profile must create it
        const TestProfile::Profile p = TestProfile::fromRoot(path(QStringLiteral("tp_lock")), true);
        QVERIFY2(p.active, qPrintable(p.error));
        QVERIFY(QFileInfo(p.dataDirectory).isDir());
        QVERIFY(!QFileInfo::exists(p.databasePath)); // only the folder, not the database

        // reusing the profile keeps the marker and the folder
        const TestProfile::Profile again = TestProfile::fromRoot(p.root, true);
        QVERIFY2(again.active, qPrintable(again.error));
        QCOMPARE(again.databasePath, p.databasePath);
        QCOMPARE(again.registryBase, p.registryBase);

        const TestProfile::Profile other = TestProfile::fromRoot(path(QStringLiteral("tp_lock2")), true);
        SingleInstanceGuard first, sameProfile, otherProfile;
        const QString lock = p.databasePath + QStringLiteral(".lock");
        QVERIFY(first.acquire(lock));
        QVERIFY(!sameProfile.acquire(lock));                      // same profile: turned away
        QCOMPARE(sameProfile.ownerPid(), qint64(QCoreApplication::applicationPid()));
        QVERIFY(otherProfile.acquire(other.databasePath + QStringLiteral(".lock"))); // another profile: independent
        first.release();
        sameProfile.release();
        otherProfile.release();
    }

    void testprofile_instanceIdentityComesFromTheDatabaseLockNotTheExecutableName()
    {
        const TestProfile::Profile a = TestProfile::fromRoot(path(QStringLiteral("tp_id_a")), true);
        const TestProfile::Profile b = TestProfile::fromRoot(path(QStringLiteral("tp_id_b")), true);
        QVERIFY(a.active && b.active);
        const QString lockA = a.databasePath + QStringLiteral(".lock");
        const QString lockB = b.databasePath + QStringLiteral(".lock");

        // nothing running on either profile
        QVERIFY(!NativeHost::isInstanceRunning(lockA));
        QVERIFY(!NativeHost::isInstanceRunning(QString()));

        // a live lock held by a process that is not TaskNest.exe (this test
        // program) does not count, so a reused process id cannot fake a TaskNest
        SingleInstanceGuard guard;
        QVERIFY(guard.acquire(lockA));
        QVERIFY(!NativeHost::isInstanceRunning(lockA));

        // a damaged lock file is "not running", never an error
        QFile junk(lockB);
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("not a lock");
        junk.close();
        QVERIFY(!NativeHost::isInstanceRunning(lockB));
        guard.release();

        // the host of a profile asks about that profile's lock only
        const NativeHost::Options oa = NativeHost::optionsFromArguments(
            {QStringLiteral("h"), QStringLiteral("--test-profile"), a.root});
        const NativeHost::Options ob = NativeHost::optionsFromArguments(
            {QStringLiteral("h"), QStringLiteral("--test-profile"), b.root});
        QCOMPARE(oa.instanceLockFile, lockA);
        QCOMPARE(ob.instanceLockFile, lockB);
        QVERIFY(oa.instanceLockFile != ob.instanceLockFile);
        // production options never name a profile's lock
        QVERIFY(NativeHost::optionsFromArguments({QStringLiteral("h")}).instanceLockFile.isEmpty());

        // with no discovery file and no live TaskNest on the profile the answer is
        // "not running", whatever else is running on the machine
        NativeHost::Options none = oa;
        QCOMPARE(codeOf(NativeHost::handle("{\"op\":\"status\"}", none)), QStringLiteral("not_running"));
    }

    // ---- instance identity: the positive owner path ------------------------------

    void instanceOwner_aLiveTaskNestOwnerOfTheLockIsRecognizedAndNothingElseIs()
    {
        const QString helper = QStringLiteral(TASKNEST_LOCKOWNER_EXE);
        QVERIFY2(QFileInfo::exists(helper), qPrintable(helper));
        QVERIFY(QDir().mkpath(path(QStringLiteral("owner"))));
        const QString asTaskNest = path(QStringLiteral("owner/TaskNest.exe"));
        const QString notTaskNest = path(QStringLiteral("owner/Unrelated.exe"));
        QVERIFY(QFile::copy(helper, asTaskNest));
        QVERIFY(QFile::copy(helper, notTaskNest));

        const TestProfile::Profile a = TestProfile::fromRoot(path(QStringLiteral("own_a")), true);
        const TestProfile::Profile b = TestProfile::fromRoot(path(QStringLiteral("own_b")), true);
        QVERIFY(a.active && b.active);
        const QString lockA = a.databasePath + QStringLiteral(".lock");
        const QString lockB = b.databasePath + QStringLiteral(".lock");
        const bool productionBefore = NativeHost::isTaskNestRunning();

        {   // 1. the live owner, named TaskNest.exe, holding profile A's lock
            LockOwner owner;
            QVERIFY(owner.start(asTaskNest, lockA));
            QVERIFY(NativeHost::isInstanceRunning(lockA));
            QVERIFY(!NativeHost::isInstanceRunning(lockB));    // another profile is not fooled
            // the host reports from the lock of its own profile only
            const auto codeFor = [](const TestProfile::Profile &p) {
                const NativeHost::Options o = NativeHost::optionsFromArguments(
                    {QStringLiteral("h"), QStringLiteral("--test-profile"), p.root});
                return codeOf(NativeHost::handle("{\"op\":\"status\"}", o));
            };
            QCOMPARE(codeFor(a), QStringLiteral("integration_disabled")); // running, no discovery file
            QCOMPARE(codeFor(b), QStringLiteral("not_running"));
            // the production host does not consult any profile's lock
            for (const QString &file : NativeHost::productionLockFiles()) {
                QVERIFY(!file.startsWith(a.root, Qt::CaseInsensitive));
                QVERIFY(!file.startsWith(b.root, Qt::CaseInsensitive));
            }
            QCOMPARE(NativeHost::isTaskNestRunning(), productionBefore);
            // the lock names this very process
            qint64 pid = 0;
            QString host, app;
            QVERIFY(QLockFile(lockA).getLockInfo(&pid, &host, &app));
            QCOMPARE(pid, owner.pid());
            // a clean exit releases the lock
            owner.stop();
            QVERIFY(!QFileInfo::exists(lockA));
            QVERIFY(!NativeHost::isInstanceRunning(lockA));
        }

        {   // 2. a crashed owner leaves a stale lock naming a dead process id
            LockOwner owner;
            QVERIFY(owner.start(asTaskNest, lockA));
            QVERIFY(NativeHost::isInstanceRunning(lockA));
            owner.crash();
            QVERIFY(QFileInfo::exists(lockA));                  // left behind
            QVERIFY(!NativeHost::isInstanceRunning(lockA));     // dead pid: not running
        }
        QVERIFY(QFile::remove(lockA));

        {   // 3. a live process that is not TaskNest.exe, even with a valid lock
            LockOwner owner;
            QVERIFY(owner.start(notTaskNest, lockA));
            QVERIFY(owner.process.state() == QProcess::Running);
            QVERIFY(!NativeHost::isInstanceRunning(lockA));
            owner.stop();
        }

        // 4. malformed or hostile lock contents fail safe
        const QList<QByteArray> contents = {
            QByteArray(), QByteArray("garbage"), QByteArray("\n\n\n"), QByteArray("abc\nTaskNest\nhost\n"),
            QByteArray("0\nTaskNest\nhost\n"), QByteArray("-7\nTaskNest\nhost\n"),
            QByteArray("99999999\nTaskNest\nhost\n"), QByteArray("4294967296\nTaskNest\nhost\n"),
            QByteArray("9223372036854775807\nTaskNest\nhost\n"), QByteArray(100000, 'x'),
        };
        for (const QByteArray &content : contents) {
            QFile f(lockB);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(content);
            f.close();
            QVERIFY2(!NativeHost::isInstanceRunning(lockB), content.left(30).constData());
        }
        QVERIFY(!NativeHost::isInstanceRunning(path(QStringLiteral("no/such/lock"))));
        QCOMPARE(NativeHost::isTaskNestRunning(), productionBefore);
    }

    // ---- the real executable under a scratch profile --------------------------------

    void app_scratchProfileRealExecutableSmokeTest()
    {
        const QString exe = QStringLiteral(TASKNEST_APP_EXE);
        QVERIFY2(QFileInfo::exists(exe), qPrintable(exe));
        const TestProfile::Profile a = TestProfile::fromRoot(path(QStringLiteral("smoke_a")), true);
        const TestProfile::Profile b = TestProfile::fromRoot(path(QStringLiteral("smoke_b")), true);
        QVERIFY(a.active && b.active);
        // nowhere near the real data
        for (const QString &protectedDir : TestProfile::protectedDirectories()) {
            QVERIFY(!a.root.startsWith(protectedDir, Qt::CaseInsensitive));
            QVERIFY(!b.root.startsWith(protectedDir, Qt::CaseInsensitive));
        }
        // the profile asks for the integration endpoint (a stored setting, as the user's switch would)
        for (const TestProfile::Profile &p : {a, b}) {
            Database db(QStringLiteral("smoke_seed_") + QString::number(p.root.size()) + p.root.right(1));
            TaskRepository repo(db);
            QVERIFY(db.initialize(p.databasePath));
            QVERIFY(repo.writeMetadata(QStringLiteral("integration_enabled"), QStringLiteral("1")));
        }

        const auto optionsFor = [](const TestProfile::Profile &p) {
            NativeHost::Options o = NativeHost::optionsFromArguments(
                {QStringLiteral("h"), QStringLiteral("--test-profile"), p.root});
            o.timeoutMs = 5000;
            return o;
        };
        const auto ask = [](const QJsonObject &message, const NativeHost::Options &o) {
            return NativeHost::handle(QJsonDocument(message).toJson(QJsonDocument::Compact), o);
        };
        const auto discoveryFiles = [](const TestProfile::Profile &p) {
            return QDir(p.discoveryDirectory).entryList({QStringLiteral("integration-*.json")}, QDir::Files);
        };
        const QJsonObject capture{{"op", "capture"}, {"title", "Smoke capture"},
                                  {"url", "https://example.com/smoke?x=1"},
                                  {"text", "smoke text <b>literal</b>"}, {"request_id", "smoke-req-1"}};

        qint64 firstTaskId = 0;
        {
            AppProcess first, second, other;
            first.start(exe, a.root);
            QTRY_VERIFY_WITH_TIMEOUT(first.running() && discoveryFiles(a).size() == 1, 30000);
            QVERIFY(QFileInfo::exists(a.databasePath));
            QVERIFY(QFileInfo::exists(a.databasePath + QStringLiteral(".lock")));
            QVERIFY(QFileInfo::exists(QDir(a.root).filePath(QLatin1String(TestProfile::kMarkerFile))));
            QVERIFY(!QFileInfo::exists(QDir(b.discoveryDirectory).filePath(discoveryFiles(a).first())));

            // a second launch of the same profile is turned away and owns nothing
            qint64 ownerPid = 0;
            QString h, an;
            QVERIFY(QLockFile(a.databasePath + QStringLiteral(".lock")).getLockInfo(&ownerPid, &h, &an));
            QCOMPARE(ownerPid, first.process.processId());
            second.start(exe, a.root);
            QVERIFY(second.process.waitForFinished(30000));
            QCOMPARE(second.process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(second.process.exitCode(), 0);
            QVERIFY(first.running());
            QVERIFY(QLockFile(a.databasePath + QStringLiteral(".lock")).getLockInfo(&ownerPid, &h, &an));
            QCOMPARE(ownerPid, first.process.processId());
            QCOMPARE(discoveryFiles(a).size(), 1);

            // a different profile runs alongside, with its own endpoint and database
            other.start(exe, b.root);
            QTRY_VERIFY_WITH_TIMEOUT(other.running() && discoveryFiles(b).size() == 1, 30000);
            QVERIFY(discoveryFiles(a).first() != discoveryFiles(b).first());

            // capture through the host of profile A: one task, retried id is the same task
            const QJsonObject status = ask({{"op", "status"}}, optionsFor(a));
            QVERIFY2(status.value("ok").toBool(), qPrintable(codeOf(status)));
            const QJsonObject r1 = ask(capture, optionsFor(a));
            QVERIFY2(r1.value("ok").toBool(), qPrintable(codeOf(r1)));
            firstTaskId = static_cast<qint64>(r1.value("result").toObject().value("task_id").toDouble());
            QVERIFY(firstTaskId > 0);
            const QJsonObject r2 = ask(capture, optionsFor(a));
            QVERIFY(r2.value("ok").toBool());
            QVERIFY(r2.value("result").toObject().value("duplicate").toBool());
            QCOMPARE(static_cast<qint64>(r2.value("result").toObject().value("task_id").toDouble()), firstTaskId);
            QJsonObject other_ = capture;
            other_.insert("request_id", "smoke-req-2");
            const QJsonObject r3 = ask(other_, optionsFor(a));
            QVERIFY(r3.value("ok").toBool());
            QVERIFY(static_cast<qint64>(r3.value("result").toObject().value("task_id").toDouble()) != firstTaskId);
            // profile B has not seen any of it
            QVERIFY(ask({{"op", "status"}}, optionsFor(b)).value("ok").toBool());

            // closing releases the lock and removes the discovery file
            QVERIFY(first.requestQuit());
            const bool closed = first.process.waitForFinished(15000);
            other.end();
            if (!closed) {
                first.end();
                QSKIP("the offscreen test process could not be closed gracefully; "
                      "lock and discovery cleanup were not verified");
            }
            QCOMPARE(first.process.exitCode(), 0);
            QVERIFY(!QFileInfo::exists(a.databasePath + QStringLiteral(".lock")));
            QCOMPARE(discoveryFiles(a).size(), 0);
        }

        // the scratch database holds the captures with their source address
        {
            Database db(QStringLiteral("smoke_check"));
            TaskRepository repo(db);
            QVERIFY(db.initialize(a.databasePath));
            QList<Task> tasks;
            QVERIFY(repo.loadTasks({}, &tasks));
            QCOMPARE(tasks.size(), 2);
            Task t;
            QVERIFY(repo.getTask(firstTaskId, &t));
            QCOMPARE(t.title, QStringLiteral("Smoke capture"));
            QVERIFY(t.notes.endsWith(QStringLiteral("Source: https://example.com/smoke?x=1")));
            QVERIFY(t.notes.contains(QStringLiteral("<b>literal</b>")));
        }
        {
            Database db(QStringLiteral("smoke_check_b"));
            TaskRepository repo(db);
            QVERIFY(db.initialize(b.databasePath));
            QList<Task> tasks;
            QVERIFY(repo.loadTasks({}, &tasks));
            QCOMPARE(tasks.size(), 0);
        }
    }

    void app_invalidProfileOptionsExitWithCodeTwoBeforeTouchingAnyDatabase()
    {
        const QString exe = QStringLiteral(TASKNEST_APP_EXE);
        QVERIFY(QFileInfo::exists(exe));
        const QString foreign = path(QStringLiteral("exit2_foreign"));
        QVERIFY(QDir().mkpath(foreign));
        QFile stray(QDir(foreign).filePath(QStringLiteral("notes.txt")));
        QVERIFY(stray.open(QIODevice::WriteOnly));
        stray.close();
        const QList<QStringList> bad = {
            {QStringLiteral("--test-profile"), QStringLiteral("relative/folder")},
            {QStringLiteral("--test-profile"), QStringLiteral("//server/share/profile")},
            {QStringLiteral("--test-profile"), QStringLiteral("C:/")},
            {QStringLiteral("--test-profile"), QStringLiteral("C:/scratch/../x")},
            {QStringLiteral("--test-profile"), foreign},
            {QStringLiteral("--test-profile")},
        };
        for (const QStringList &args : bad) {
            AppProcess app;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            app.process.setProcessEnvironment(env);
            app.process.setProgram(exe);
            app.process.setArguments(args);
            app.process.start();
            QVERIFY2(app.process.waitForFinished(30000), qPrintable(args.join(' ')));
            QCOMPARE(app.process.exitCode(), 2);
        }
        QVERIFY(!QFileInfo::exists(QDir(foreign).filePath(QLatin1String(TestProfile::kMarkerFile))));
        QVERIFY(!QFileInfo::exists(QDir(foreign).filePath(QStringLiteral("data"))));
    }

#ifdef Q_OS_WIN
    void testprofile_registrationUsesOnlyTheProfileSubtree()
    {
        const TestProfile::Profile p = TestProfile::fromRoot(path(QStringLiteral("tp_reg")), true);
        QVERIFY2(p.active, qPrintable(p.error));
        const QString hostExe = path(QStringLiteral("tp_reg_TaskNestHost.exe"));
        QFile fake(hostExe);
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("MZ");
        fake.close();

        // the real keys, and their state, before
        const QString realChrome = QStringLiteral(
            "HKEY_CURRENT_USER\\Software\\Google\\Chrome\\NativeMessagingHosts\\com.tasknest.host");
        const QString realEdge = QStringLiteral(
            "HKEY_CURRENT_USER\\Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.tasknest.host");
        QString chromeBefore, edgeBefore;
        const bool chromeHad = RegistryUtil::readDefault(realChrome, &chromeBefore);
        const bool edgeHad = RegistryUtil::readDefault(realEdge, &edgeBefore);

        NativeHostRegistration reg = NativeHostRegistration::forProfile(p);
        for (const QString &key : reg.registryKeys())
            QVERIFY2(key.startsWith(p.registryBase), qPrintable(key));
        QVERIFY(reg.manifestPath().startsWith(p.manifestDirectory));

        // an unrelated entry in the same subtree must survive
        const QString sibling = p.registryBase
                                + QStringLiteral("\\Google\\Chrome\\NativeMessagingHosts\\other.host");
        QSettings(sibling, QSettings::NativeFormat).setValue(QStringLiteral("Default"), QStringLiteral("x"));

        QString why;
        QVERIFY2(reg.registerHost(hostExe, &why), qPrintable(why));
        QVERIFY(reg.isRegistered(hostExe));
        QFile launcher(reg.launcherPath());
        QVERIFY(launcher.open(QIODevice::ReadOnly));
        const QString script = QString::fromLocal8Bit(launcher.readAll());
        launcher.close();
        QVERIFY(script.contains(QStringLiteral("--test-profile \"%1\"")
                                    .arg(QDir::toNativeSeparators(p.root))));
        QVERIFY(!script.contains(QStringLiteral("token"), Qt::CaseInsensitive));
        QFile manifest(reg.manifestPath());
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const QJsonObject m = QJsonDocument::fromJson(manifest.readAll()).object();
        manifest.close();
        QCOMPARE(m.value("allowed_origins").toArray().size(), 1);
        QCOMPARE(m.value("allowed_origins").toArray().first().toString(), NativeHost::extensionOrigin());
        QCOMPARE(QDir::fromNativeSeparators(m.value("path").toString()),
                 QDir::fromNativeSeparators(reg.launcherPath()));

        // the real registrations did not move
        QString chromeAfter, edgeAfter;
        QCOMPARE(RegistryUtil::readDefault(realChrome, &chromeAfter), chromeHad);
        QCOMPARE(RegistryUtil::readDefault(realEdge, &edgeAfter), edgeHad);
        QCOMPARE(chromeAfter, chromeBefore);
        QCOMPARE(edgeAfter, edgeBefore);

        QVERIFY(reg.unregisterHost());
        QVERIFY(!reg.isRegistered(hostExe));
        QVERIFY(!QFileInfo::exists(reg.manifestPath()));
        QVERIFY(!QFileInfo::exists(reg.launcherPath()));
        QVERIFY(QFileInfo::exists(hostExe));
        QVERIFY(RegistryUtil::keyExists(sibling)); // unrelated entry preserved

        // an inactive profile, or one whose base is a real browser key, can never register
        TestProfile::Profile bogus = p;
        bogus.active = false;
        QVERIFY(!NativeHostRegistration::forProfile(bogus).registerHost(hostExe, &why));
        bogus = p;
        bogus.registryBase = QStringLiteral("HKEY_CURRENT_USER\\Software\\Google\\Chrome\\NativeMessagingHosts");
        QVERIFY(!TestProfile::isProfileRegistryBase(bogus.registryBase));
        QVERIFY(!NativeHostRegistration::forProfile(bogus).registerHost(hostExe, &why));
        QString realAfter;
        QCOMPARE(RegistryUtil::readDefault(realChrome, &realAfter), chromeHad);

        // clean up only what this test created
        RegistryUtil::deleteTree(p.registryBase);
        QSettings parent(QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTestProfiles"),
                         QSettings::NativeFormat);
        if (parent.childGroups().isEmpty() && parent.childKeys().isEmpty())
            parent.remove(QString());
    }
#endif
};

QTEST_GUILESS_MAIN(TestTaskNest)

#include "tst_tasknest.moc"
