#pragma once

#include <QString>

// Owns the application's single named SQLite connection (Qt SQL, QSQLITE).
//
// Storage policy (see defaultDatabasePath()):
//   1. <exe dir>/data/tasknest.db when that directory is writable (portable mode)
//   2. otherwise <AppDataLocation>/tasknest.db
// An existing database is never replaced, reset or deleted by this class.
class Database
{
public:
    // Version written to PRAGMA user_version and app_metadata('schema_version').
    static constexpr int kSchemaVersion = 1;

    explicit Database(const QString &connectionName = QStringLiteral("tasknest_main"));
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    // Opens the database and creates/validates the schema. Pass an explicit
    // path to bypass the storage policy (used for tests). Returns true only
    // when the connection is open and the schema is valid. Safe to call again.
    bool initialize(const QString &databasePath = QString());

    // Closes the connection and removes the named Qt SQL connection.
    void shutdown();

    bool isReady() const { return m_ready; }
    QString databasePath() const { return m_databasePath; }
    QString lastError() const { return m_lastError; }
    QString connectionName() const { return m_connectionName; }
    int schemaVersion() const { return m_schemaVersion; }

    // Resolves where the database should live. On failure returns an empty
    // string and, if given, fills *error.
    static QString defaultDatabasePath(QString *error = nullptr);

private:
    bool openConnection(const QString &path);
    bool migrate();
    void closeConnection();
    bool fail(const QString &message);

    QString m_connectionName;
    QString m_databasePath;
    QString m_lastError;
    int m_schemaVersion = 0;
    bool m_ready = false;
};
