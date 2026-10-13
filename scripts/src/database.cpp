#include "database.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryFile>
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

    const bool portableWritable = isWritableDirectory(portableDir);

    // Keep using whichever database already exists so a changed environment
    // never silently starts a second, empty database.
    if (portableWritable && QFileInfo::exists(portablePath))
        return portablePath;
    if (!userPath.isEmpty() && QFileInfo::exists(userPath) && isWritableDirectory(userDir))
        return userPath;
    if (portableWritable)
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
        if (!db.transaction())
            return fail(QStringLiteral("Cannot start transaction: %1").arg(db.lastError().text()));

        bool ok = true;
        if (version < 1)
            ok = createSchemaV1(db, &error);
        if (ok && version < 2)
            ok = createSchemaV2(db, &error);
        // Future migrations: `if (ok && version < 3) ok = createSchemaV3(db, &error);`
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
