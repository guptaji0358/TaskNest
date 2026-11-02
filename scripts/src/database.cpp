#include "database.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariant>

namespace {

const QString kFileName = QStringLiteral("tasknest.db");
const QString kSchemaVersionKey = QStringLiteral("schema_version");

// Creates the directory if needed and proves it is writable by creating (and
// automatically removing) a temporary file. QFileInfo::isWritable is not
// reliable for Windows directories.
bool isWritableDirectory(const QString &dirPath)
{
    if (dirPath.isEmpty() || !QDir().mkpath(dirPath))
        return false;
    QTemporaryFile probe(QDir(dirPath).filePath(QStringLiteral("tasknest_probe_XXXXXX")));
    return probe.open();
}

bool runStatement(QSqlDatabase &db, const QString &sql, QString *error)
{
    QSqlQuery query(db);
    if (!query.exec(sql)) {
        if (error)
            *error = QStringLiteral("%1 (%2)").arg(query.lastError().text(), sql);
        return false;
    }
    return true;
}

bool readUserVersion(QSqlDatabase &db, int *version, QString *error)
{
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    *version = query.value(0).toInt();
    return true;
}

bool createSchemaV1(QSqlDatabase &db, QString *error)
{
    return runStatement(db,
                        QStringLiteral("CREATE TABLE IF NOT EXISTS app_metadata ("
                                       "key TEXT PRIMARY KEY, "
                                       "value TEXT NOT NULL)"),
                        error);
}

// v2: tasks table. Timestamps are ISO 8601 UTC text, so they sort correctly.
bool createSchemaV2(QSqlDatabase &db, QString *error)
{
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS tasks ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "title TEXT NOT NULL CHECK (length(trim(title)) > 0), "
                       "notes TEXT NOT NULL DEFAULT '', "
                       "list_name TEXT NOT NULL DEFAULT 'Inbox', "
                       "priority INTEGER NOT NULL DEFAULT 0 CHECK (priority BETWEEN 0 AND 2), "
                       "due_at TEXT, "
                       "completed INTEGER NOT NULL DEFAULT 0 CHECK (completed IN (0, 1)), "
                       "reminder_at TEXT, "
                       "reminder_sent INTEGER NOT NULL DEFAULT 0 CHECK (reminder_sent IN (0, 1)), "
                       "created_at TEXT NOT NULL, "
                       "updated_at TEXT NOT NULL)"),
        // Due-date views/sorting only ever look at tasks that have a due date.
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tasks_due_at "
                       "ON tasks (due_at) WHERE due_at IS NOT NULL"),
        // Reminder scheduling needs "next unsent reminder of an open task".
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tasks_reminder_pending "
                       "ON tasks (reminder_at) "
                       "WHERE reminder_at IS NOT NULL AND reminder_sent = 0 AND completed = 0"),
    };
    for (const QString &statement : statements) {
        if (!runStatement(db, statement, error))
            return false;
    }
    return true;
}

// v3: completion timestamp, the basis for "delete completed tasks after N
// days". Non-destructive: existing rows keep every value. Tasks that are
// already completed get updated_at as their completion time; that is never
// earlier than the real completion, so a retention rule can only delete them
// later than it otherwise would, never sooner.
bool createSchemaV3(QSqlDatabase &db, QString *error)
{
    const QStringList statements = {
        QStringLiteral("ALTER TABLE tasks ADD COLUMN completed_at TEXT"),
        QStringLiteral("UPDATE tasks SET completed_at = updated_at WHERE completed = 1"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tasks_completed_at "
                       "ON tasks (completed_at) WHERE completed = 1"),
    };
    for (const QString &statement : statements) {
        if (!runStatement(db, statement, error))
            return false;
    }
    return true;
}

// v4: cloud-sync groundwork (docs: api/SYNC.md section 11). Purely additive:
// no existing column or row is changed except that every task gets a UUID in
// the new `uuid` column. Local integer ids are untouched. Nothing here talks to
// a network, and nothing is recorded in the outbox until an account is linked.
//   tasks.uuid            identity across devices (unique, never null after this step)
//   tasks.server_rev      revision of the last adopted server copy (null = never on a server)
//   tasks.held/_rev/_deleted_at   "deleted in cloud, kept here" state (11.8)
//   tasks.not_synced      reason an operation was refused for content reasons (11.8)
//   sync_state            key/value: linked, link_generation, epoch, cursor, device_id
//   sync_base             full last-server-copy snapshot per task (11.10)
//   sync_outbox           journal of operations not yet acknowledged (11.2)
//   sync_tombstones       local record of tasks this device deleted (11.5)
//   sync_conflicts        values this device did not keep, and server notices (11.12)
bool createSchemaV4(QSqlDatabase &db, QString *error)
{
    int rowsBefore = 0;
    {
        QSqlQuery count(db);
        if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM tasks")) || !count.next()) {
            if (error)
                *error = count.lastError().text();
            return false;
        }
        rowsBefore = count.value(0).toInt();
    }

    const QStringList statements = {
        QStringLiteral("ALTER TABLE tasks ADD COLUMN uuid TEXT"),
        QStringLiteral("ALTER TABLE tasks ADD COLUMN server_rev INTEGER"),
        QStringLiteral("ALTER TABLE tasks ADD COLUMN held TEXT"),
        QStringLiteral("ALTER TABLE tasks ADD COLUMN held_rev INTEGER"),
        QStringLiteral("ALTER TABLE tasks ADD COLUMN held_deleted_at TEXT"),
        QStringLiteral("ALTER TABLE tasks ADD COLUMN not_synced TEXT"),
    };
    for (const QString &statement : statements) {
        if (!runStatement(db, statement, error))
            return false;
    }

    // One fresh random UUID per existing row, in the same transaction as the
    // schema change; the integer id stays the row's primary key.
    QList<qint64> ids;
    {
        QSqlQuery select(db);
        if (!select.exec(QStringLiteral("SELECT id FROM tasks ORDER BY id"))) {
            if (error)
                *error = select.lastError().text();
            return false;
        }
        while (select.next())
            ids << select.value(0).toLongLong();
    }
    for (qint64 id : ids) {
        QSqlQuery update(db);
        if (!update.prepare(QStringLiteral("UPDATE tasks SET uuid = ? WHERE id = ? AND uuid IS NULL"))) {
            if (error)
                *error = update.lastError().text();
            return false;
        }
        update.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
        update.addBindValue(id);
        if (!update.exec() || update.numRowsAffected() != 1) {
            if (error)
                *error = QStringLiteral("Cannot assign an identity to task %1: %2")
                             .arg(id).arg(update.lastError().text());
            return false;
        }
    }

    const QStringList tables = {
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_uuid ON tasks (uuid)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_state ("
                       "key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_base ("
                       "task_uuid TEXT PRIMARY KEY, "
                       "rev INTEGER NOT NULL, "
                       "snapshot TEXT NOT NULL, "
                       "epoch TEXT, "
                       "trusted INTEGER NOT NULL DEFAULT 1 CHECK (trusted IN (0, 1)))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_outbox ("
                       "seq INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "op_id TEXT NOT NULL UNIQUE, "
                       "type TEXT NOT NULL CHECK (type IN ('create', 'update', 'delete', 'restore', 'set_setting')), "
                       "task_uuid TEXT, "
                       "setting_key TEXT, "
                       "base_rev INTEGER, "
                       "after_op_id TEXT, "
                       "state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending', 'in_flight')), "
                       "changed TEXT, "
                       "task TEXT, "
                       "reason TEXT, "
                       "value TEXT, "
                       "authored_rev INTEGER, "
                       "collided INTEGER NOT NULL DEFAULT 0 CHECK (collided IN (0, 1)), "
                       "restore_of TEXT, "
                       "attempts INTEGER NOT NULL DEFAULT 0, "
                       "queued_at TEXT NOT NULL, "
                       "sent_payload TEXT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sync_outbox_task ON sync_outbox (task_uuid, seq)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_tombstones ("
                       "task_uuid TEXT PRIMARY KEY, "
                       "local_id INTEGER, "
                       "server_rev INTEGER, "
                       "deleted_at TEXT NOT NULL, "
                       "reason TEXT NOT NULL, "
                       "tombstone_rev INTEGER, "
                       "held_rev INTEGER, "
                       "held_deleted_at TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_conflicts ("
                       "id TEXT PRIMARY KEY, "
                       "record_uid TEXT, "
                       "source TEXT NOT NULL CHECK (source IN ('local', 'server')), "
                       "kind TEXT NOT NULL, "
                       "task_uuid TEXT, "
                       "setting_key TEXT, "
                       "field_group TEXT, "
                       "mine_value TEXT, "
                       "theirs_value TEXT, "
                       "base_value TEXT, "
                       "server_rev_seen INTEGER, "
                       "other_device TEXT, "
                       "detected_at TEXT NOT NULL, "
                       "change_seq INTEGER, "
                       "delete_reason TEXT, "
                       "deleted_at TEXT, "
                       "state TEXT NOT NULL DEFAULT 'open' CHECK (state IN ('open', 'kept_cloud', 'restored')), "
                       "restore_op_id TEXT, "
                       "resolved_at TEXT, "
                       "notified INTEGER NOT NULL DEFAULT 0 CHECK (notified IN (0, 1)))"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_sync_conflicts_uid "
                       "ON sync_conflicts (record_uid) WHERE record_uid IS NOT NULL"),
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_sync_conflicts_local "
                       "ON sync_conflicts (kind, COALESCE(task_uuid, ''), COALESCE(setting_key, ''), "
                       "COALESCE(field_group, ''), COALESCE(server_rev_seen, -1)) WHERE source = 'local'"),
    };
    for (const QString &statement : tables) {
        if (!runStatement(db, statement, error))
            return false;
    }

    // The migration must not lose or invent a row, and every row needs an identity.
    QSqlQuery check(db);
    if (!check.exec(QStringLiteral("SELECT COUNT(*), COUNT(uuid), COUNT(DISTINCT uuid) FROM tasks"))
        || !check.next()) {
        if (error)
            *error = check.lastError().text();
        return false;
    }
    if (check.value(0).toInt() != rowsBefore || check.value(1).toInt() != rowsBefore
        || check.value(2).toInt() != rowsBefore) {
        if (error)
            *error = QStringLiteral("Migration check failed: %1 tasks before, %2 after, %3 with a unique identity")
                         .arg(rowsBefore).arg(check.value(0).toInt()).arg(check.value(2).toInt());
        return false;
    }
    return true;
}

bool writeSchemaVersion(QSqlDatabase &db, int version, QString *error)
{
    {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "INSERT INTO app_metadata (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        query.addBindValue(kSchemaVersionKey);
        query.addBindValue(QString::number(version));
        if (!query.exec()) {
            if (error)
                *error = query.lastError().text();
            return false;
        }
    }
    // PRAGMA statements cannot take bound parameters; `version` is an int constant.
    return runStatement(db, QStringLiteral("PRAGMA user_version = %1").arg(version), error);
}

// Confirms app_metadata exists and records the expected schema version.
bool verifySchema(QSqlDatabase &db, int expectedVersion, QString *error)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT value FROM app_metadata WHERE key = ?"));
    query.addBindValue(kSchemaVersionKey);
    if (!query.exec() || !query.next()) {
        if (error)
            *error = QStringLiteral("Schema verification failed: %1")
                         .arg(query.lastError().isValid() ? query.lastError().text()
                                                          : QStringLiteral("schema_version missing"));
        return false;
    }
    if (query.value(0).toString() != QString::number(expectedVersion)) {
        if (error)
            *error = QStringLiteral("Schema verification failed: app_metadata records version %1, expected %2")
                         .arg(query.value(0).toString())
                         .arg(expectedVersion);
        return false;
    }

    if (expectedVersion >= 2) {
        QSqlQuery tables(db);
        if (!tables.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'tasks'"))
            || !tables.next()) {
            if (error)
                *error = QStringLiteral("Schema verification failed: tasks table is missing");
            return false;
        }
    }
    if (expectedVersion >= 3) {
        QSqlQuery columns(db);
        bool hasCompletedAt = false;
        if (columns.exec(QStringLiteral("PRAGMA table_info(tasks)"))) {
            while (columns.next()) {
                if (columns.value(1).toString() == QStringLiteral("completed_at"))
                    hasCompletedAt = true;
            }
        }
        if (!hasCompletedAt) {
            if (error)
                *error = QStringLiteral("Schema verification failed: tasks.completed_at is missing");
            return false;
        }
    }
    if (expectedVersion >= 4) {
        const QStringList columns = {QStringLiteral("uuid"), QStringLiteral("server_rev"), QStringLiteral("held"),
                                     QStringLiteral("held_rev"), QStringLiteral("held_deleted_at"),
                                     QStringLiteral("not_synced")};
        QStringList present;
        QSqlQuery info(db);
        if (info.exec(QStringLiteral("PRAGMA table_info(tasks)"))) {
            while (info.next())
                present << info.value(1).toString();
        }
        for (const QString &column : columns) {
            if (!present.contains(column)) {
                if (error)
                    *error = QStringLiteral("Schema verification failed: tasks.%1 is missing").arg(column);
                return false;
            }
        }
        for (const QString &table : {QStringLiteral("sync_state"), QStringLiteral("sync_base"),
                                     QStringLiteral("sync_outbox"), QStringLiteral("sync_tombstones"),
                                     QStringLiteral("sync_conflicts")}) {
            QSqlQuery exists(db);
            exists.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?"));
            exists.addBindValue(table);
            if (!exists.exec() || !exists.next()) {
                if (error)
                    *error = QStringLiteral("Schema verification failed: %1 is missing").arg(table);
                return false;
            }
        }
        // The tombstone record remembers the hold of a task deleted while its Keep was in flight.
        QSqlQuery tombColumns(db);
        QStringList tombPresent;
        if (tombColumns.exec(QStringLiteral("PRAGMA table_info(sync_tombstones)"))) {
            while (tombColumns.next())
                tombPresent << tombColumns.value(1).toString();
        }
        for (const QString &column : {QStringLiteral("held_rev"), QStringLiteral("held_deleted_at")}) {
            if (!tombPresent.contains(column)) {
                if (error)
                    *error = QStringLiteral("Schema verification failed: sync_tombstones.%1 is missing").arg(column);
                return false;
            }
        }
        QSqlQuery missing(db);
        if (!missing.exec(QStringLiteral("SELECT COUNT(*) FROM tasks WHERE uuid IS NULL")) || !missing.next()
            || missing.value(0).toInt() != 0) {
            if (error)
                *error = QStringLiteral("Schema verification failed: a task has no identity");
            return false;
        }
    }
    return true;
}

} // namespace

Database::Database(const QString &connectionName)
    : m_connectionName(connectionName)
{
}

Database::~Database()
{
    shutdown();
}

QString Database::defaultDatabasePath(QString *error)
{
    const QString portableDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
    const QString portablePath = QDir(portableDir).filePath(kFileName);

    const QString userDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString userPath = userDir.isEmpty() ? QString() : QDir(userDir).filePath(kFileName);

    // 1. Keep using whichever database already exists, so a changed environment
    //    never silently starts a second, empty database.
    if (QFileInfo::exists(portablePath)) {
        if (isWritableDirectory(portableDir))
            return portablePath;
        // The data is there but cannot be used: say so instead of quietly
        // creating an empty database somewhere else.
        if (error) {
            *error = QStringLiteral("A database exists at \"%1\" but its folder is not writable.")
                         .arg(QDir::toNativeSeparators(portablePath));
        }
        return QString();
    }
    if (!userPath.isEmpty() && QFileInfo::exists(userPath)) {
        if (isWritableDirectory(userDir))
            return userPath;
        if (error) {
            *error = QStringLiteral("A database exists at \"%1\" but its folder is not writable.")
                         .arg(QDir::toNativeSeparators(userPath));
        }
        return QString();
    }

    // 2. First run: portable (beside the executable) if possible, else per-user.
    //    The data/ folder is only created here, when it is actually going to be used.
    if (isWritableDirectory(portableDir))
        return portablePath;
    if (!userPath.isEmpty() && isWritableDirectory(userDir))
        return userPath;

    if (error) {
        *error = QStringLiteral("No writable storage location found (tried \"%1\" and \"%2\").")
                     .arg(QDir::toNativeSeparators(portableDir), QDir::toNativeSeparators(userDir));
    }
    return QString();
}

bool Database::fail(const QString &message)
{
    m_lastError = message;
    m_ready = false;
    return false;
}

bool Database::initialize(const QString &databasePath)
{
    if (m_ready)
        return true;

    m_lastError.clear();
    m_schemaVersion = 0;

    QString path = databasePath;
    if (path.isEmpty()) {
        QString error;
        path = defaultDatabasePath(&error);
        if (path.isEmpty())
            return fail(error);
    }
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    m_databasePath = path;

    const QFileInfo info(path);
    if (info.isDir())
        return fail(QStringLiteral("Database path is a directory: %1").arg(QDir::toNativeSeparators(path)));
    if (!info.dir().exists() && !QDir().mkpath(info.absolutePath()))
        return fail(QStringLiteral("Cannot create database directory: %1")
                        .arg(QDir::toNativeSeparators(info.absolutePath())));

    // Checked before opening so we never tear down a connection we do not own.
    if (QSqlDatabase::contains(m_connectionName))
        return fail(QStringLiteral("Database connection \"%1\" already exists.").arg(m_connectionName));

    if (!openConnection(path) || !migrate()) {
        // Keep the original error; closing the connection must not mask it.
        const QString error = m_lastError;
        closeConnection();
        return fail(error);
    }

    m_ready = true;
    return true;
}

bool Database::openConnection(const QString &path)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(path);
    if (!db.open()) {
        return fail(QStringLiteral("Cannot open database %1: %2")
                        .arg(QDir::toNativeSeparators(path), db.lastError().text()));
    }
    return true;
}

bool Database::migrate()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    QString error;

    if (!runStatement(db, QStringLiteral("PRAGMA foreign_keys = ON"), &error))
        return fail(error);

    // The first real read also detects files that are not SQLite databases.
    int version = 0;
    if (!readUserVersion(db, &version, &error)) {
        return fail(QStringLiteral("%1 is not a usable SQLite database: %2")
                        .arg(QDir::toNativeSeparators(m_databasePath), error));
    }

    if (version > kSchemaVersion) {
        return fail(QStringLiteral("Database schema version %1 is newer than this application supports (%2). "
                                   "The database was left untouched.")
                        .arg(version)
                        .arg(kSchemaVersion));
    }

    if (version < kSchemaVersion) {
        // Upgrading an existing database: keep a copy of it exactly as it was
        // (once per source version) before the schema is touched. If the copy
        // cannot be made, the upgrade is not attempted.
        if (version > 0) {
            const QString backup = QStringLiteral("%1.v%2.bak").arg(m_databasePath).arg(version);
            if (!QFileInfo(backup).isFile() && !QFile::copy(m_databasePath, backup)) {
                return fail(QStringLiteral("Cannot back up the database to \"%1\" before upgrading it. "
                                           "The database was left untouched.")
                                .arg(QDir::toNativeSeparators(backup)));
            }
        }

        if (!db.transaction())
            return fail(QStringLiteral("Cannot start transaction: %1").arg(db.lastError().text()));

        bool ok = true;
        if (version < 1)
            ok = createSchemaV1(db, &error);
        if (ok && version < 2)
            ok = createSchemaV2(db, &error);
        if (ok && version < 3)
            ok = createSchemaV3(db, &error);
        if (ok && version < 4)
            ok = createSchemaV4(db, &error);
        if (ok)
            ok = writeSchemaVersion(db, kSchemaVersion, &error);

        if (!ok) {
            db.rollback();
            return fail(QStringLiteral("Schema creation failed: %1").arg(error));
        }
        if (!db.commit()) {
            const QString commitError = db.lastError().text();
            db.rollback();
            return fail(QStringLiteral("Cannot commit schema changes: %1").arg(commitError));
        }
    }

    if (!verifySchema(db, kSchemaVersion, &error))
        return fail(error);

    m_schemaVersion = kSchemaVersion;
    return true;
}

QSqlDatabase Database::connection() const
{
    return QSqlDatabase::database(m_connectionName, false);
}

void Database::closeConnection()
{
    if (!QSqlDatabase::contains(m_connectionName))
        return;

    // The handle must go out of scope before removeDatabase(), otherwise Qt
    // warns about a connection that is still in use.
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

void Database::shutdown()
{
    closeConnection();
    m_ready = false;
}
