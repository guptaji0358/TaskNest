#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>

class Database;

// Local storage side of the optional cloud sync (api/SYNC.md, section 11).
// There is no network code here: this class only decides what is written to,
// and read from, the local SQLite file. Everything that mutates several tables
// does so inside one transaction owned by the caller (the record* functions)
// or by the function itself (apply*, reconcile*, resolve*).
//
// Nothing is recorded until an account is linked (setLinked); with sync off the
// record* functions are not called and TaskNest behaves exactly as before.
//
// Tasks travel as JSON objects with exactly the fields of SYNC 2.1:
// id (the UUID), title, notes, list_name, priority, due_at, completed,
// completed_at, reminder_at, created_at.

// RAII transaction (BEGIN IMMEDIATE ... COMMIT). Rolls back on destruction
// unless commit() succeeded. Not re-entrant: SQLite has no nested BEGIN.
class DbTransaction
{
public:
    explicit DbTransaction(QSqlDatabase db);
    ~DbTransaction();
    DbTransaction(const DbTransaction &) = delete;
    DbTransaction &operator=(const DbTransaction &) = delete;

    bool begin();
    bool commit();
    void rollback();
    QString error() const { return m_error; }

private:
    QSqlDatabase m_db;
    bool m_active = false;
    QString m_error;
};

class TestSync;

class SyncStore
{
public:
    enum class AckOutcome {
        Committed,
        RolledBack,
        UnknownOutcome,   // response incomplete or malformed: nothing applied
        EpochChanged,     // different server epoch: nothing applied
        Dropped,          // link generation changed (unlink/account switch)
        Ignored,          // duplicate delivery: no matching in-flight rows
        CrashedBeforeApply,
        CommittedThenCrashed,
    };
    struct ConflictResolution
    {
        QString result;         // kept_cloud, queued, needs_confirm, unavailable, deleted
        QJsonValue current;     // needs_confirm: the value the device shows now
    };

    explicit SyncStore(Database &database);

    // ---- link state ------------------------------------------------------
    bool isLinked() const;
    // Linking or unlinking bumps the link generation (SYNC 11.11).
    bool setLinked(bool linked, const QString &deviceId = QString());
    QString stateValue(const QString &key, const QString &fallback = QString()) const;
    bool setStateValue(const QString &key, const QString &value);

    // Operation ids come from here; tests replace it with fixed ids.
    void setOpIdGenerator(std::function<QString()> generator) { m_opId = std::move(generator); }

    // ---- recording local changes (caller's transaction) -----------------
    // All of these are no-ops (returning true) while sync is off.
    bool recordCreate(const QJsonObject &task);
    // `before`/`after` are the task as it was and is. Writes one `update` for
    // the groups that differ (or folds into an unsent row); nothing when none.
    // `restoreOf` (a conflict record id) makes the row a never-folded restore row;
    // `opIdOut` receives the id of the row written, if any.
    bool recordEdit(const QJsonObject &before, const QJsonObject &after,
                    const QString &restoreOf = QString(), QString *opIdOut = nullptr);
    // `reason`: user, retention or clear_completed. Call BEFORE the row is removed
    // (it reads the task's revision and held state), inside the same transaction.
    bool recordDelete(const QJsonObject &task, qint64 localId, const QString &reason);
    // Undo of a delete: cancels the unsent delete, or queues a restore.
    bool recordUndoDelete(const QJsonObject &task, bool *cancelled = nullptr);

    // Applies the write rules of 11.2 (folding, waiting behind an in-flight row)
    // to a fully specified operation in its wire form, plus optional `after`,
    // `authored_rev` and `restore_of`. The recorders above build on the same path.
    bool enqueueOperation(const QJsonObject &operation);

    // ---- batches (no network) -------------------------------------------
    // Compacts unsent rows (11.4), then returns the in-flight rows
    // byte-identical followed by up to `limit` newly sendable ones, which are
    // marked in flight in the same transaction. Each entry is one wire operation.
    bool prepareBatch(QList<QByteArray> *payloads, int limit = 200);

    // ---- applying server state ------------------------------------------
    // One pull page: items (task/tombstone/setting), conflict records and the
    // new cursor in a single transaction (11.5). `cursor` null = leave as is.
    enum class PullOutcome {
        Applied,
        EpochChanged,   // the page belongs to another server epoch: nothing applied
        NotLinked,
        Failed,         // malformed (no epoch) or a storage error: nothing applied
    };
    // The same, for a page that arrived with `serverEpoch` (SYNC 5.2, 10).
    PullOutcome applyPullPage(const QJsonArray &items, const QJsonArray &conflicts, const QVariant &cursor,
                              const QString &serverEpoch);
    // The whole 200 response of a push, in one transaction (11.11).
    AckOutcome applyPushResponse(const QJsonObject &response, qint64 requestLinkGeneration)
    {
        return applyPushResponse(response, requestLinkGeneration, AckFault());
    }
    // 9.2 for one task whose server state is `item` (a pull item).
    bool reconcileTask(const QJsonObject &item, bool failBeforeCommit = false);

    // ---- held tasks (11.8) ----------------------------------------------
    bool holdKeep(const QString &uuid);
    bool holdDeleteLocal(const QString &uuid);
    // After a restore was answered invalid_base_rev. *ask: ask the user again;
    // *reconcile: the task is live elsewhere, run 9.2.
    bool holdRestoreRejected(const QString &uuid, bool serverTombstoned, int tombstoneRev,
                             bool *ask, bool *reconcile);

    // ---- conflict records (11.12) ---------------------------------------
    bool storeConflict(const QJsonObject &record);
    bool resolveConflict(const QString &id, const QString &action, const QString &now,
                         ConflictResolution *resolution);
    bool cleanupConflicts(const QString &now);
    // Local tombstone records (and bases) of deleted tasks older than 30 days that nothing
    // queued refers to. Like cleanupConflicts, to be called by the sync driver; never on a timer.
    bool cleanupTombstones(const QString &now);

    // ---- helpers ---------------------------------------------------------
    static const QStringList &groups();
    static QJsonValue groupValue(const QJsonObject &task, const QString &group);
    static QStringList differingGroups(const QJsonObject &a, const QJsonObject &b);
    static QByteArray compactJson(const QJsonObject &object);

    QString lastError() const { return m_lastError; }

private:
    // Test-only access (tests/tst_sync.cpp): the epoch-less applyPullPage and the AckFault
    // overload of applyPushResponse below. Production code cannot reach either.
    friend class TestSync;

    // Where simulated failures happen when applying a push response (tests). Every field
    // defaults to "no fault"; the public 2-argument applyPushResponse passes a default one.
    struct AckFault
    {
        bool beforeApply = false;   // response read, transaction not started
        int duringResult = 0;       // 1-based result at which applying fails
        bool beforeCommit = false;
        bool afterCommit = false;
    };
    // Applies a page without looking at an epoch (conformance fixtures only). The public
    // overload with `serverEpoch` is the only form production code may call.
    bool applyPullPage(const QJsonArray &items, const QJsonArray &conflicts, const QVariant &cursor);
    AckOutcome applyPushResponse(const QJsonObject &response, qint64 requestLinkGeneration,
                                 const AckFault &fault);

    struct Row;
    struct TaskInfo;

    QSqlDatabase db() const;
    bool fail(const QString &message);
    QString newOpId() const;

    bool loadTask(const QString &uuid, QJsonObject *task, TaskInfo *info = nullptr) const;
    bool taskInfo(const QString &uuid, TaskInfo *info) const;
    bool loadBase(const QString &uuid, QJsonObject *snapshot, int *rev = nullptr) const;
    bool storeBase(const QString &uuid, int rev, const QJsonObject &snapshot);
    bool writeTaskFields(const QString &uuid, const QJsonObject &task);
    bool insertTask(const QJsonObject &task, int rev);
    bool deleteTaskRows(const QString &uuid);
    bool setServerRev(const QString &uuid, const QVariant &rev);

    QList<Row> rowsForTask(const QString &uuid) const;
    QList<Row> allRows() const;
    QList<Row> queryRows(const QString &where, const QVariantList &binds) const;
    // True unless the task's base was distrusted (epoch change, repeated invalid_base_rev):
    // such a revision is never used as a base_rev (SYNC 10, 11.x).
    bool baseTrusted(const QString &uuid) const;
    bool insertRow(Row &row);
    bool updateRow(const Row &row);
    bool deleteRow(const QString &opId);
    bool writeOperation(Row row);
    bool resolveDependents(const Row &acked, const QJsonObject &server, const QJsonObject &oldBase,
                           bool haveOldBase, int storedRev, const QStringList &ackedGroups);
    QJsonObject wirePayload(const Row &row) const;

    bool reconcileInTx(const QJsonObject &item);
    // 9.2 for `candidates` only: `merged` receives the surviving values, `carried` the
    // groups this device alone changed (to be sent). Differing groups the server also
    // changed (or with no base) are kept as the server's and preserved in a record.
    bool reconcileGroups(const QString &uuid, const QJsonObject &local, const QJsonObject &server, int rev,
                         const QStringList &candidates, QJsonObject *merged, QStringList *carried);
    bool resultIsValid(const Row &row, const QJsonObject &result) const;
    // A restore row decided against older cloud data is dropped when an acknowledgement
    // brings a different cloud value for its group (11.12, strict restore rows).
    bool dropCollidedRestores(const Row &acked, const QJsonObject &server, int rev, const QJsonObject &oldBase,
                              bool haveOldBase, const QStringList &ackedGroups);
    // Deleting a held task locally: a queued restore must never bring it back (11.8).
    bool discardHeldRows(const QString &uuid, qint64 localId, const QString &reason);
    bool applyPullPageImpl(const QJsonArray &items, const QJsonArray &conflicts, const QVariant &cursor,
                           const QString *serverEpoch, PullOutcome *outcome = nullptr);
    bool applyTaskItem(const QJsonObject &item);
    bool applyTombstoneItem(const QJsonObject &item);
    bool applyResult(const Row &row, const QJsonObject &result);
    bool reopenRestoreRecord(const QString &recordId);
    bool dropRowsOfTask(const QString &uuid, const QString &afterOp);
    bool storeServerConflicts(const QJsonArray &conflicts);
    bool insertConflict(const QJsonObject &record, bool *inserted = nullptr);
    bool setHeld(const QString &uuid, int tombstoneRev, const QString &deletedAt);
    bool replaceWithServerCopy(const QString &uuid, const QJsonObject &task, int rev);

    Database &m_database;
    QString m_lastError;
    std::function<QString()> m_opId;
};
