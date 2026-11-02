// Tests for schema v4, the local sync store and its integration into the
// repository (api/SYNC.md section 11). Every test uses temporary databases.
// The shared fixtures in api/conformance/client_cases.json are read in place.

#include "database.h"
#include "syncstore.h"
#include "task.h"
#include "taskrepository.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

namespace {

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

// Runs statements on a private connection (used to seed and inspect files).
struct RawDb
{
    explicit RawDb(const QString &path)
        : name(QUuid::createUuid().toString())
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        opened = db.open();
    }
    ~RawDb()
    {
        {
            QSqlDatabase db = QSqlDatabase::database(name, false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    bool exec(const QString &sql, const QVariantList &binds = {})
    {
        QSqlQuery query(QSqlDatabase::database(name, false));
        query.prepare(sql);
        for (const QVariant &v : binds)
            query.addBindValue(v);
        const bool ok = query.exec();
        if (!ok)
            lastError = query.lastError().text();
        return ok;
    }
    QVariant scalar(const QString &sql)
    {
        QSqlQuery query(QSqlDatabase::database(name, false));
        return query.exec(sql) && query.next() ? query.value(0) : QVariant();
    }
    QList<QVariantList> rows(const QString &sql)
    {
        QList<QVariantList> result;
        QSqlQuery query(QSqlDatabase::database(name, false));
        if (!query.exec(sql))
            return result;
        const int columns = query.record().count();
        while (query.next()) {
            QVariantList row;
            for (int i = 0; i < columns; ++i)
                row << query.value(i);
            result << row;
        }
        return result;
    }
    QString name;
    bool opened = false;
    QString lastError;
};

const char *kV3Columns = "id, title, notes, list_name, priority, due_at, completed, reminder_at, "
                         "reminder_sent, created_at, updated_at, completed_at";

// A schema-v3 database as shipped before the cloud work.
void createV3(const QString &path, int taskCount)
{
    RawDb db(path);
    QVERIFY(db.opened);
    const QStringList ddl = {
        "CREATE TABLE app_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
        "INSERT INTO app_metadata VALUES ('schema_version', '3')",
        "INSERT INTO app_metadata VALUES ('completed_cleanup_policy', 'immediately')",
        "CREATE TABLE tasks (id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "title TEXT NOT NULL CHECK (length(trim(title)) > 0), notes TEXT NOT NULL DEFAULT '', "
        "list_name TEXT NOT NULL DEFAULT 'Inbox', "
        "priority INTEGER NOT NULL DEFAULT 0 CHECK (priority BETWEEN 0 AND 2), due_at TEXT, "
        "completed INTEGER NOT NULL DEFAULT 0 CHECK (completed IN (0, 1)), reminder_at TEXT, "
        "reminder_sent INTEGER NOT NULL DEFAULT 0 CHECK (reminder_sent IN (0, 1)), "
        "created_at TEXT NOT NULL, updated_at TEXT NOT NULL, completed_at TEXT)",
        "CREATE INDEX idx_tasks_due_at ON tasks (due_at) WHERE due_at IS NOT NULL",
        "CREATE INDEX idx_tasks_completed_at ON tasks (completed_at) WHERE completed = 1",
    };
    for (const QString &s : ddl)
        QVERIFY2(db.exec(s), qPrintable(db.lastError));
    for (int i = 1; i <= taskCount; ++i) {
        const bool done = i % 3 == 0;
        QVERIFY2(db.exec(QStringLiteral("INSERT INTO tasks (title, notes, list_name, priority, due_at, completed, "
                                        "reminder_at, reminder_sent, created_at, updated_at, completed_at) "
                                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
                          {QStringLiteral("Aufgabe Über %1 ✓").arg(i), QStringLiteral("notes\nline %1 'q' \"d\"").arg(i),
                           i % 2 ? QStringLiteral("Inbox") : QStringLiteral("Work"), i % 3,
                           i % 4 ? QVariant(QStringLiteral("2026-05-0%1T10:00:00.000Z").arg(i % 9)) : QVariant(QMetaType::fromType<QString>()),
                           done ? 1 : 0, QVariant(QMetaType::fromType<QString>()), 0,
                           QStringLiteral("2026-01-01T10:00:00.000Z"), QStringLiteral("2026-01-02T10:00:00.000Z"),
                           done ? QVariant(QStringLiteral("2026-01-02T10:00:00.000Z")) : QVariant(QMetaType::fromType<QString>())}),
                 qPrintable(db.lastError));
    }
    // A gap in the ids, as left by deleted tasks: AUTOINCREMENT must keep counting.
    QVERIFY(db.exec("DELETE FROM tasks WHERE id = 2"));
    QVERIFY(db.exec("PRAGMA user_version = 3"));
}

// ---- fixture helpers -----------------------------------------------------

QJsonObject defaultTask(const QString &id)
{
    return {{"id", id}, {"title", "Task"}, {"notes", ""}, {"list_name", "Inbox"}, {"priority", 0},
            {"due_at", QJsonValue::Null}, {"completed", false}, {"completed_at", QJsonValue::Null},
            {"reminder_at", QJsonValue::Null}, {"created_at", "2026-01-01T10:00:00.000Z"}};
}

QJsonObject fillTask(const QJsonObject &partial, const QString &id)
{
    QJsonObject task = defaultTask(id);
    for (auto it = partial.begin(); it != partial.end(); ++it)
        task.insert(it.key(), it.value());
    task.insert("id", id);
    return task;
}

QString subset(const QJsonValue &expect, const QJsonValue &actual, const QString &path)
{
    if (expect.isString() && expect.toString() == QLatin1String("$any"))
        return {};
    if (expect.isObject()) {
        if (!actual.isObject())
            return path + ": expected an object";
        const QJsonObject e = expect.toObject(), a = actual.toObject();
        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it.value().isString() && it.value().toString() == QLatin1String("$absent")) {
                if (a.contains(it.key()))
                    return path + "/" + it.key() + ": expected to be absent";
                continue;
            }
            if (!a.contains(it.key()))
                return path + "/" + it.key() + ": missing";
            const QString error = subset(it.value(), a.value(it.key()), path + "/" + it.key());
            if (!error.isEmpty())
                return error;
        }
        return {};
    }
    if (expect.isArray()) {
        if (!actual.isArray())
            return path + ": expected an array";
        const QJsonArray e = expect.toArray(), a = actual.toArray();
        if (e.size() != a.size())
            return QStringLiteral("%1: expected %2 items, got %3 (%4)")
                .arg(path).arg(e.size()).arg(a.size()).arg(QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
        for (int i = 0; i < e.size(); ++i) {
            const QString error = subset(e.at(i), a.at(i), path + "[" + QString::number(i) + "]");
            if (!error.isEmpty())
                return error;
        }
        return {};
    }
    if (expect != actual) {
        return QStringLiteral("%1: expected %2, got %3")
            .arg(path, QString::fromUtf8(QJsonDocument(QJsonArray{expect}).toJson(QJsonDocument::Compact)),
                 QString::fromUtf8(QJsonDocument(QJsonArray{actual}).toJson(QJsonDocument::Compact)));
    }
    return {};
}

QJsonValue sqlJson(const QVariant &v)
{
    return v.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue::fromVariant(v);
}

QVariant nullStr() { return QVariant(QMetaType::fromType<QString>()); }
QVariant nullInt() { return QVariant(QMetaType::fromType<qlonglong>()); }

QVariant jsonSql(const QJsonValue &v)
{
    if (v.isNull() || v.isUndefined())
        return nullStr();
    if (v.isBool())
        return v.toBool() ? 1 : 0;
    if (v.isDouble())
        return static_cast<qlonglong>(v.toDouble());
    return v.toString();
}

QString compact(const QJsonValue &v)
{
    return v.isObject() ? QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact))
                        : QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
}

// A scratch database loaded from a fixture's `local` block and read back in the same shape.
struct Fixture
{
    QTemporaryDir dir;
    Database database;
    TaskRepository repo;
    QString path;

    Fixture()
        : database(QStringLiteral("tst_sync_fixture_%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
        , repo(database)
    {
        path = dir.filePath("t.db");
        if (!database.initialize(path))
            qFatal("Fixture: cannot initialize the scratch database");
    }
    ~Fixture() { database.shutdown(); }

    bool sql(const QString &s, const QVariantList &binds = {})
    {
        QSqlQuery q(database.connection());
        q.prepare(s);
        for (const QVariant &b : binds)
            q.addBindValue(b);
        const bool ok = q.exec();
        if (!ok)
            qWarning() << q.lastError().text() << s;
        return ok;
    }

    void load(const QJsonObject &local)
    {
        SyncStore &store = repo.sync();
        QVERIFY(store.setLinked(true, local.value("device_id").toString()));
        if (local.contains("link_generation"))
            QVERIFY(store.setStateValue("link_generation", QString::number(local.value("link_generation").toInt())));
        if (local.contains("epoch"))
            QVERIFY(store.setStateValue("epoch", local.value("epoch").toString()));
        if (local.contains("cursor"))
            QVERIFY(store.setStateValue("cursor", QString::number(local.value("cursor").toInt())));

        const QJsonObject tasks = local.value("tasks").toObject();
        for (auto it = tasks.begin(); it != tasks.end(); ++it) {
            const QString uuid = it.key();
            const QJsonObject entry = it.value().toObject();
            const QJsonObject t = fillTask(entry.value("task").toObject(), uuid);
            const QJsonObject held = entry.value("held").toObject();
            QVERIFY(sql("INSERT INTO tasks (uuid, title, notes, list_name, priority, due_at, completed, completed_at, "
                        "reminder_at, reminder_sent, created_at, updated_at, server_rev, held, held_rev, not_synced) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?, ?)",
                        {uuid, t["title"].toString(), t["notes"].toString(), t["list_name"].toString(), t["priority"].toInt(),
                         jsonSql(t["due_at"]), t["completed"].toBool() ? 1 : 0, jsonSql(t["completed_at"]),
                         jsonSql(t["reminder_at"]), t["created_at"].toString(), "2026-01-01T10:00:00.000Z",
                         entry.contains("server_rev") ? QVariant(entry.value("server_rev").toInt()) : nullInt(),
                         held.isEmpty() ? nullStr() : QVariant(held.value("reason").toString()),
                         held.isEmpty() ? nullInt() : QVariant(held.value("tombstone_rev").toInt()),
                         entry.contains("not_synced") ? QVariant(entry.value("not_synced").toString()) : nullStr()}));
            if (entry.contains("base")) {
                QVERIFY(sql("INSERT INTO sync_base (task_uuid, rev, snapshot, trusted) VALUES (?, ?, ?, 1)",
                            {uuid, entry.value("server_rev").toInt(),
                             compact(fillTask(entry.value("base").toObject().value("task").toObject(), uuid))}));
            }
        }
        for (const QJsonValue &v : local.value("outbox").toArray()) {
            const QJsonObject r = v.toObject();
            const QString uuid = r.value("task_id").toString();
            QStringList changed;
            for (const QJsonValue &g : r.value("changed").toArray())
                changed << g.toString();
            changed.sort();
            QJsonArray changedJson;
            for (const QString &g : changed)
                changedJson.append(g);
            QVERIFY(sql("INSERT INTO sync_outbox (op_id, type, task_uuid, base_rev, after_op_id, state, changed, task, "
                        "reason, authored_rev, collided, restore_of, attempts, queued_at, sent_payload) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        {r.value("op_id").toString(), r.value("type").toString(), uuid,
                         r.value("base_rev").isNull() || !r.contains("base_rev") ? nullInt() : QVariant(r.value("base_rev").toInt()),
                         r.contains("after") ? QVariant(r.value("after").toString()) : nullStr(),
                         r.value("in_flight").toBool() ? "in_flight" : "pending",
                         changed.isEmpty() ? nullStr() : QVariant(compact(changedJson)),
                         r.contains("task") ? QVariant(compact(fillTask(r.value("task").toObject(), uuid))) : nullStr(),
                         r.contains("reason") ? QVariant(r.value("reason").toString()) : nullStr(),
                         r.contains("authored_rev") ? QVariant(r.value("authored_rev").toInt()) : nullInt(),
                         r.value("collided").toBool() ? 1 : 0,
                         r.contains("restore_of") ? QVariant(r.value("restore_of").toString()) : nullStr(),
                         r.value("attempts").toInt(), "2026-01-01T00:00:00.000Z",
                         r.contains("sent_payload") ? QVariant(r.value("sent_payload").toString()) : nullStr()}));
        }
        for (const QJsonValue &v : local.value("conflicts").toArray())
            QVERIFY(store.storeConflict(v.toObject()));
    }

    QJsonObject read()
    {
        QJsonObject out;
        SyncStore &store = repo.sync();
        out["cursor"] = store.stateValue("cursor").toInt();
        out["epoch"] = store.stateValue("epoch");
        QJsonObject tasks;
        QSqlQuery q(database.connection());
        q.exec("SELECT uuid, title, notes, list_name, priority, due_at, completed, completed_at, reminder_at, "
               "created_at, server_rev, held, held_rev, not_synced FROM tasks");
        while (q.next()) {
            const QString uuid = q.value(0).toString();
            QJsonObject entry;
            entry["task"] = QJsonObject{{"id", uuid}, {"title", q.value(1).toString()}, {"notes", q.value(2).toString()},
                                        {"list_name", q.value(3).toString()}, {"priority", q.value(4).toInt()},
                                        {"due_at", sqlJson(q.value(5))}, {"completed", q.value(6).toInt() != 0},
                                        {"completed_at", sqlJson(q.value(7))}, {"reminder_at", sqlJson(q.value(8))},
                                        {"created_at", q.value(9).toString()}};
            entry["server_rev"] = sqlJson(q.value(10));
            if (!q.value(11).isNull())
                entry["held"] = QJsonObject{{"reason", q.value(11).toString()}, {"tombstone_rev", q.value(12).toInt()}};
            if (!q.value(13).isNull())
                entry["not_synced"] = q.value(13).toString();
            tasks[uuid] = entry;
        }
        QSqlQuery b(database.connection());
        b.exec("SELECT task_uuid, snapshot FROM sync_base");
        while (b.next()) {
            QJsonObject entry = tasks.value(b.value(0).toString()).toObject();
            entry["base"] = QJsonObject{{"task", QJsonDocument::fromJson(b.value(1).toString().toUtf8()).object()}};
            tasks[b.value(0).toString()] = entry;
        }
        out["tasks"] = tasks;

        QJsonArray outbox;
        QSqlQuery o(database.connection());
        o.exec("SELECT op_id, type, task_uuid, base_rev, after_op_id, state, changed, task, reason, authored_rev, "
               "collided, restore_of, attempts, sent_payload FROM sync_outbox ORDER BY seq");
        while (o.next()) {
            QJsonObject r{{"op_id", o.value(0).toString()}, {"type", o.value(1).toString()},
                          {"task_id", o.value(2).toString()}, {"base_rev", sqlJson(o.value(3))},
                          {"in_flight", o.value(5).toString() == "in_flight"}, {"authored_rev", sqlJson(o.value(9))},
                          {"collided", o.value(10).toInt() != 0}, {"attempts", o.value(12).toInt()}};
            if (!o.value(4).isNull())
                r["after"] = o.value(4).toString();
            if (!o.value(6).isNull())
                r["changed"] = QJsonDocument::fromJson(o.value(6).toString().toUtf8()).array();
            if (!o.value(7).isNull())
                r["task"] = QJsonDocument::fromJson(o.value(7).toString().toUtf8()).object();
            if (!o.value(8).isNull())
                r["reason"] = o.value(8).toString();
            if (!o.value(11).isNull())
                r["restore_of"] = o.value(11).toString();
            if (!o.value(13).isNull())
                r["sent_payload"] = o.value(13).toString();
            outbox.append(r);
        }
        out["outbox"] = outbox;
        out["conflicts"] = readConflicts();
        return out;
    }

    QJsonArray readConflicts()
    {
        QJsonArray conflicts;
        QSqlQuery c(database.connection());
        c.exec("SELECT id, record_uid, source, kind, task_uuid, field_group, mine_value, theirs_value, base_value, "
               "server_rev_seen, state, restore_op_id, resolved_at, detected_at FROM sync_conflicts ORDER BY rowid");
        auto val = [](const QVariant &v) {
            if (v.isNull())
                return QJsonValue(QJsonValue::Null);
            const QJsonArray a = QJsonDocument::fromJson(v.toString().toUtf8()).array();
            return a.isEmpty() ? QJsonValue(QJsonValue::Null) : a.at(0);
        };
        while (c.next()) {
            conflicts.append(QJsonObject{{"id", c.value(0).toString()}, {"record_uid", sqlJson(c.value(1))},
                                         {"source", c.value(2).toString()}, {"kind", c.value(3).toString()},
                                         {"task_id", sqlJson(c.value(4))}, {"field_group", sqlJson(c.value(5))},
                                         {"mine_value", val(c.value(6))}, {"theirs_value", val(c.value(7))},
                                         {"base_value", val(c.value(8))}, {"server_rev_seen", sqlJson(c.value(9))},
                                         {"state", c.value(10).toString()}, {"restore_op_id", sqlJson(c.value(11))},
                                         {"resolved_at", sqlJson(c.value(12))}, {"detected_at", c.value(13).toString()}});
        }
        return conflicts;
    }

    QStringList inFlightOps()
    {
        QStringList ids;
        QSqlQuery o(database.connection());
        o.exec("SELECT op_id FROM sync_outbox WHERE state = 'in_flight' ORDER BY seq");
        while (o.next())
            ids << o.value(0).toString();
        return ids;
    }

    qint64 idOf(const QString &uuid)
    {
        QSqlQuery q(database.connection());
        q.prepare("SELECT id FROM tasks WHERE uuid = ?");
        q.addBindValue(uuid);
        return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
    }
    int count(const QString &table)
    {
        QSqlQuery q(database.connection());
        return q.exec("SELECT COUNT(*) FROM " + table) && q.next() ? q.value(0).toInt() : -1;
    }
};

QJsonObject fillItem(QJsonObject item)
{
    if (item.contains("task"))
        item["task"] = fillTask(item.value("task").toObject(), item.value("task_id").toString());
    return item;
}

QJsonObject fillResult(QJsonObject result, const QString &opId, const QString &uuid)
{
    result["op_id"] = opId;
    if (result.contains("task"))
        result["task"] = fillTask(result.value("task").toObject(), uuid);
    return result;
}

QString outcomeName(SyncStore::AckOutcome o)
{
    using O = SyncStore::AckOutcome;
    switch (o) {
    case O::Committed: return "committed";
    case O::RolledBack: return "rolled_back";
    case O::UnknownOutcome: return "unknown_outcome";
    case O::EpochChanged: return "epoch_changed";
    case O::Dropped: return "dropped";
    case O::Ignored: return "ignored";
    case O::CrashedBeforeApply: return "crashed_before_apply";
    case O::CommittedThenCrashed: return "committed_then_crashed";
    }
    return {};
}

QJsonObject loadCases()
{
    QFile file(QStringLiteral(TASKNEST_SOURCE_DIR "/api/conformance/client_cases.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

const QStringList kImplementedKinds = {
    "outbox_write", "outbox_undo_delete", "outbox_compact", "apply_pull", "apply_result", "apply_response",
    "apply_response_seq", "reconcile_apply", "held_transition", "local_mutation", "local_edit",
    "conflict_store", "conflict_resolve", "conflict_cleanup"};

} // namespace

class TestSync : public QObject
{
    Q_OBJECT

private slots:
    // ---- migration -------------------------------------------------------
    void migrationKeepsEveryRowAndAssignsIdentities();
    void migrationOfFailingDatabaseLeavesItUntouched();
    void freshDatabaseIsV4();
    void migrationOfRealDatabaseCopy();

    // ---- mutation integration -------------------------------------------
    void unlinkedMutationsRecordNothing();
    void linkedMutationsWriteOneOperationEach();
    void markReminderSentIsLocalOnly();
    void deleteUndoAndCompaction();
    void sentDeleteUndoQueuesRestore();
    void purgeWritesTombstonesWithReasons();
    void failedMutationLeavesNothingBehind();
    void heldTaskQueuesNothing();
    void firstEpochIsPersisted();

    // ---- M1 corrective pass (F1-F11) -------------------------------------
    void keepThenEditPreservesTheEdit();
    void deleteAfterKeepNeverRecreatesTheTask();
    void acknowledgementsWithUnknownStatusChangeNothing();
    void notDeletedReconcilesInsteadOfOverwriting();
    void restoreRowIsDroppedWhenAnAcknowledgementBringsNewCloudData();
    void revisionsOnlyMoveForward();
    void editsMadeWhileUnlinkedSurviveAPull();
    void notSyncedAndKeepEdgeCases();
    void conflictRecordsAreGuardedWhileARestoreIsQueued();
    void tombstonesNeverRemoveUnsentContent();

    // ---- M1.2: undo while a restore is in flight, invalid_base_rev --------
    void undoDuringInFlightRestoreKeepsTheHold();
    void invalidBaseRevRetriesOnceUnderANewOpId();
    void restoreRejectedWithInvalidBaseRevIsNeverRetried();

    // ---- M1.3: not_synced paths, conflict resolution, review items ------
    void notSyncedWithoutABaseQueuesACreate();
    void notSyncedEndsWhenTheTaskIsBackAtItsBase();
    void notSyncedSurvivesDeleteAndUndo();
    void distrustedBaseIsNeverUsedAsABaseRev();
    void repeatedInvalidBaseRevDistrustsTheBase();
    void conflictRecordIsGuardedWhileTheRestoreIsInFlight();
    void conflictRecordIsGuardedByItsRestoreOpId();
    void unknownTaskLeavesNoTombstoneOrBase();
    void oldTombstonesAreCollected();
    void pullPagesAreCheckedAgainstTheEpoch();
    void heldTaskSurvivesUnlinkAndRelink();
    void outboxLookupByTaskUsesTheIndex();

    // ---- shared conformance fixtures ------------------------------------
    void conformance_data();
    void conformance();
    void conformanceCoversTheWholeFixtureFile();
};

// ===========================================================================
// Migration
// ===========================================================================

void TestSync::migrationKeepsEveryRowAndAssignsIdentities()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("tasknest.db");
    createV3(path, 12);
    const QByteArray original = fileHash(path);

    RawDb before(path);
    const auto rowsBefore = before.rows(QStringLiteral("SELECT %1 FROM tasks ORDER BY id").arg(kV3Columns));
    const QVariant seqBefore = before.scalar("SELECT seq FROM sqlite_sequence WHERE name = 'tasks'");
    const auto metaBefore = before.rows("SELECT key, value FROM app_metadata WHERE key <> 'schema_version' ORDER BY key");
    QCOMPARE(rowsBefore.size(), 11);

    {
        Database db;
        QVERIFY2(db.initialize(path), qPrintable(db.lastError()));
        QCOMPARE(db.schemaVersion(), 4);
    }
    // The pre-upgrade copy is byte-identical to the original file.
    QCOMPARE(fileHash(path + ".v3.bak"), original);

    RawDb after(path);
    QCOMPARE(after.scalar("PRAGMA user_version").toInt(), 4);
    QCOMPARE(after.scalar("PRAGMA integrity_check").toString(), QString("ok"));
    QCOMPARE(after.rows(QStringLiteral("SELECT %1 FROM tasks ORDER BY id").arg(kV3Columns)), rowsBefore);
    QCOMPARE(after.rows("SELECT key, value FROM app_metadata WHERE key <> 'schema_version' ORDER BY key"), metaBefore);
    QCOMPARE(after.scalar("SELECT seq FROM sqlite_sequence WHERE name = 'tasks'"), seqBefore);
    QCOMPARE(after.scalar("SELECT COUNT(DISTINCT uuid) FROM tasks").toInt(), 11);
    QCOMPARE(after.scalar("SELECT COUNT(*) FROM tasks WHERE uuid IS NULL").toInt(), 0);
    for (const QVariantList &row : after.rows("SELECT uuid FROM tasks"))
        QVERIFY(!QUuid::fromString(row.at(0).toString()).isNull());
    QCOMPARE(after.scalar("SELECT COUNT(*) FROM tasks WHERE server_rev IS NOT NULL OR held IS NOT NULL "
                          "OR not_synced IS NOT NULL").toInt(), 0);
    // Nothing is recorded for a database that was never linked.
    QCOMPARE(after.scalar("SELECT COUNT(*) FROM sync_outbox").toInt(), 0);
    QCOMPARE(after.scalar("SELECT COUNT(*) FROM sync_tombstones").toInt(), 0);

    // Ids keep counting from where they were; reopening changes nothing more.
    {
        Database db;
        QVERIFY(db.initialize(path));
        TaskRepository repo(db);
        Task t;
        t.title = "new";
        Task created;
        QVERIFY(repo.addTask(t, &created));
        QCOMPARE(created.id, seqBefore.toLongLong() + 1);
        QVERIFY(!created.uuid.isEmpty());
    }
}

void TestSync::migrationOfFailingDatabaseLeavesItUntouched()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("tasknest.db");
    createV3(path, 5);
    {
        RawDb db(path); // a column the migration wants to add already exists: the step must fail
        QVERIFY(db.exec("ALTER TABLE tasks ADD COLUMN uuid TEXT"));
    }
    RawDb before(path);
    const auto rowsBefore = before.rows("SELECT * FROM tasks ORDER BY id");
    before.exec("PRAGMA optimize");

    Database db;
    QVERIFY(!db.initialize(path));
    db.shutdown();

    RawDb after(path);
    QCOMPARE(after.scalar("PRAGMA user_version").toInt(), 3);
    QCOMPARE(after.rows("SELECT * FROM tasks ORDER BY id"), rowsBefore);
    QCOMPARE(after.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name = 'sync_outbox'").toInt(), 0);
    QVERIFY(QFileInfo(path + ".v3.bak").isFile()); // the safety copy was made first
}

void TestSync::freshDatabaseIsV4()
{
    QTemporaryDir dir;
    Database db;
    QVERIFY(db.initialize(dir.filePath("t.db")));
    QCOMPARE(db.schemaVersion(), 4);
    RawDb raw(dir.filePath("t.db"));
    for (const char *table : {"sync_state", "sync_base", "sync_outbox", "sync_tombstones", "sync_conflicts"})
        QCOMPARE(raw.scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name = '%1'").arg(table)).toInt(), 1);
}

void TestSync::migrationOfRealDatabaseCopy()
{
    // Run manually with TASKNEST_MIGRATION_SOURCE=<a COPY of a real database>.
    // The source is only read; the copy is what gets migrated.
    const QString source = qEnvironmentVariable("TASKNEST_MIGRATION_SOURCE");
    if (source.isEmpty()) {
        qInfo("TASKNEST_MIGRATION_SOURCE not set: real-database copy check not run in this invocation");
        return;
    }
    QVERIFY(QFileInfo(source).isFile());
    const QByteArray sourceHash = fileHash(source);
    QTemporaryDir dir;
    const QString copy = dir.filePath("tasknest.db");
    QVERIFY(QFile::copy(source, copy));

    RawDb before(copy);
    const int version = before.scalar("PRAGMA user_version").toInt();
    const auto rowsBefore = before.rows(QStringLiteral("SELECT %1 FROM tasks ORDER BY id").arg(kV3Columns));
    const auto metaBefore = before.rows("SELECT key, value FROM app_metadata WHERE key <> 'schema_version' ORDER BY key");
    QVERIFY2(version == 3, "the copy must be a schema-3 database");
    {
        Database db;
        QVERIFY2(db.initialize(copy), qPrintable(db.lastError()));
        QCOMPARE(db.schemaVersion(), 4);
    }
    RawDb after(copy);
    QCOMPARE(after.scalar("PRAGMA integrity_check").toString(), QString("ok"));
    QCOMPARE(after.rows(QStringLiteral("SELECT %1 FROM tasks ORDER BY id").arg(kV3Columns)), rowsBefore);
    QCOMPARE(after.rows("SELECT key, value FROM app_metadata WHERE key <> 'schema_version' ORDER BY key"), metaBefore);
    QCOMPARE(after.scalar("SELECT COUNT(DISTINCT uuid) FROM tasks").toInt(), rowsBefore.size());
    QCOMPARE(fileHash(source), sourceHash); // the original was never touched
    qInfo("real-database copy: %d tasks preserved, all with unique identities", int(rowsBefore.size()));
}

// ===========================================================================
// Mutation integration
// ===========================================================================

namespace {

struct Env
{
    QTemporaryDir dir;
    Database db;
    TaskRepository repo;
    Env()
        : db(QStringLiteral("tst_sync_env_%1").arg(QUuid::createUuid().toString(QUuid::Id128)))
        , repo(db)
    {
        if (!db.initialize(dir.filePath("t.db")))
            qFatal("Env: cannot initialize the scratch database");
    }
    ~Env() { db.shutdown(); }
    Task add(const QString &title)
    {
        Task t, out;
        t.title = title;
        if (!repo.addTask(t, &out))
            qFatal("Env: cannot add a task");
        return out;
    }
    QStringList outboxTypes()
    {
        QStringList types;
        QSqlQuery q(db.connection());
        q.exec("SELECT type FROM sync_outbox ORDER BY seq");
        while (q.next())
            types << q.value(0).toString();
        return types;
    }
    int count(const QString &table)
    {
        QSqlQuery q(db.connection());
        return q.exec("SELECT COUNT(*) FROM " + table) && q.next() ? q.value(0).toInt() : -1;
    }
    // Sends everything and has the server accept it, giving each task `rev`.
    void acknowledgeAll(int rev)
    {
        QList<QByteArray> batch;
        QVERIFY(repo.sync().prepareBatch(&batch));
        QJsonArray results;
        for (const QByteArray &payload : batch) {
            const QJsonObject op = QJsonDocument::fromJson(payload).object();
            QJsonObject result{{"op_id", op["op_id"]}, {"status", "applied"}, {"rev", rev}, {"seq", rev}};
            if (op["type"] != "delete")
                result["task"] = op["task"];
            else
                result["deleted"] = true;
            results.append(result);
        }
        QCOMPARE(repo.sync().applyPushResponse({{"server_epoch", "e1"}, {"results", results}},
                                               repo.sync().stateValue("link_generation").toLongLong()),
                 SyncStore::AckOutcome::Committed);
    }
};

qint64 linkGen(Env &env)
{
    return env.repo.sync().stateValue("link_generation").toLongLong();
}

QJsonObject okResult(const QJsonObject &op, int rev, const QString &status = "applied")
{
    QJsonObject r{{"op_id", op["op_id"]}, {"status", status}, {"rev", rev}, {"seq", rev}};
    if (op["type"] != "delete")
        r["task"] = op["task"];
    else
        r["deleted"] = true;
    return r;
}

QJsonObject responseOf(const QJsonArray &results)
{
    return {{"server_epoch", "e1"}, {"results", results}};
}

QJsonObject opOf(const QByteArray &payload)
{
    return QJsonDocument::fromJson(payload).object();
}

// Marks every task as deleted in the cloud (held), recording the tombstone revision.
void holdAll(Env &env, int tombstoneRev)
{
    QSqlQuery q(env.db.connection());
    q.prepare("UPDATE tasks SET held = 'deleted_in_cloud', held_rev = ?");
    q.addBindValue(tombstoneRev);
    q.exec();
}

QVariant scalarOf(Env &env, const QString &sql)
{
    QSqlQuery q(env.db.connection());
    return q.exec(sql) && q.next() ? q.value(0) : QVariant();
}

QJsonObject baseSnapshot(Env &env, const QString &uuid)
{
    return QJsonDocument::fromJson(scalarOf(env, "SELECT snapshot FROM sync_base WHERE task_uuid = '" + uuid + "'")
                                       .toString().toUtf8()).object();
}

QJsonObject pullTask(const QString &uuid, int rev, const QJsonObject &task)
{
    return {{"kind", "task"}, {"task_id", uuid}, {"rev", rev}, {"seq", rev}, {"task", task}};
}

QJsonObject pullTombstone(const QString &uuid, int rev)
{
    return {{"kind", "tombstone"}, {"task_id", uuid}, {"rev", rev}, {"seq", rev},
            {"deleted_at", "2026-02-01T00:00:00.000Z"}, {"delete_reason", "user"}};
}

} // namespace

void TestSync::unlinkedMutationsRecordNothing()
{
    Env env;
    Task t = env.add("a");
    t.title = "b";
    QVERIFY(env.repo.updateTask(t));
    QVERIFY(env.repo.setCompleted(t.id, true));
    QVERIFY(env.repo.deleteTask(t.id));
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(env.count("sync_tombstones"), 0);
    QCOMPARE(env.count("sync_base"), 0);
}

void TestSync::linkedMutationsWriteOneOperationEach()
{
    Env env;
    QVERIFY(env.repo.sync().setLinked(true, "dev-a"));
    Task t = env.add("first");
    QCOMPARE(env.outboxTypes(), QStringList{"create"});
    t.title = "renamed";
    QVERIFY(env.repo.updateTask(t));
    QCOMPARE(env.outboxTypes(), QStringList{"create"}); // folded into the unsent create

    env.acknowledgeAll(1);
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(env.count("sync_base"), 1);
    Task now;
    QVERIFY(env.repo.getTask(t.id, &now));

    t.priority = 2;
    QVERIFY(env.repo.updateTask(t));
    QCOMPARE(env.outboxTypes(), QStringList{"update"});
    t.listName = "Work";
    QVERIFY(env.repo.updateTask(t)); // folds: still one row
    QCOMPARE(env.outboxTypes(), QStringList{"update"});
    QVERIFY(env.repo.setCompleted(t.id, true));
    QCOMPARE(env.outboxTypes(), QStringList{"update"});
    QSqlQuery q(env.db.connection());
    q.exec("SELECT changed, base_rev, authored_rev FROM sync_outbox");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QString("[\"completion\",\"list_name\",\"priority\"]"));
    QCOMPARE(q.value(1).toInt(), 1);
    QCOMPARE(q.value(2).toInt(), 1);

    // An update that changes nothing synced writes nothing.
    const int rows = env.count("sync_outbox");
    QVERIFY(env.repo.getTask(t.id, &now));
    QVERIFY(env.repo.updateTask(now));
    QCOMPARE(env.count("sync_outbox"), rows);
    // The base never moved through a local edit.
    QSqlQuery b(env.db.connection());
    b.exec("SELECT rev, snapshot FROM sync_base");
    QVERIFY(b.next());
    QCOMPARE(b.value(0).toInt(), 1);
    QVERIFY(b.value(1).toString().contains("renamed"));
    QVERIFY(!b.value(1).toString().contains("\"priority\":2"));
}

void TestSync::markReminderSentIsLocalOnly()
{
    Env env;
    QVERIFY(env.repo.sync().setLinked(true, "dev-a"));
    Task t;
    t.title = "r";
    t.reminderAt = QDateTime::currentDateTimeUtc().addDays(-1);
    Task created;
    QVERIFY(env.repo.addTask(t, &created));
    env.acknowledgeAll(4);
    bool changed = false;
    QVERIFY(env.repo.markReminderSent(created.id, &changed));
    QVERIFY(changed);
    QCOMPARE(env.count("sync_outbox"), 0);
    QSqlQuery q(env.db.connection());
    q.exec("SELECT server_rev FROM tasks");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 4);
}

void TestSync::deleteUndoAndCompaction()
{
    Env env;
    QVERIFY(env.repo.sync().setLinked(true, "dev-a"));
    Task t = env.add("never sent");
    QVERIFY(env.repo.deleteTask(t.id));
    QCOMPARE(env.outboxTypes(), (QStringList{"create", "delete"}) ); // never on a server: only a create exists
    // create + delete is dropped entirely when the batch is built.
    QList<QByteArray> batch;
    QVERIFY(env.repo.sync().prepareBatch(&batch));
    QVERIFY(batch.isEmpty());
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(env.count("sync_tombstones"), 0);

    // Undo of an unsent delete cancels the delete row only.
    Task u = env.add("synced");
    env.acknowledgeAll(1);
    Task snapshot;
    QVERIFY(env.repo.getTask(u.id, &snapshot));
    QVERIFY(env.repo.deleteTask(u.id));
    QCOMPARE(env.outboxTypes(), QStringList{"delete"});
    QCOMPARE(env.count("sync_tombstones"), 1);
    Task restored;
    QVERIFY(env.repo.restoreTask(snapshot, &restored));
    QCOMPARE(restored.id, snapshot.id);
    QCOMPARE(restored.uuid, snapshot.uuid);
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(env.count("sync_tombstones"), 0);
    QSqlQuery q(env.db.connection());
    q.exec("SELECT server_rev FROM tasks WHERE uuid = '" + snapshot.uuid + "'");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1); // the base survived the delete
}

void TestSync::sentDeleteUndoQueuesRestore()
{
    Env env;
    QVERIFY(env.repo.sync().setLinked(true, "dev-a"));
    Task t = env.add("x");
    env.acknowledgeAll(1);
    Task snapshot;
    QVERIFY(env.repo.getTask(t.id, &snapshot));
    QVERIFY(env.repo.deleteTask(t.id));
    QList<QByteArray> batch;
    QVERIFY(env.repo.sync().prepareBatch(&batch)); // the delete is now in flight
    QCOMPARE(batch.size(), 1);

    QVERIFY(env.repo.restoreTask(snapshot));
    QCOMPARE(env.outboxTypes(), (QStringList{"delete", "restore"}));
    QSqlQuery q(env.db.connection());
    q.exec("SELECT after_op_id, base_rev FROM sync_outbox WHERE type = 'restore'");
    QVERIFY(q.next());
    QVERIFY(!q.value(0).isNull());
    QVERIFY(q.value(1).isNull()); // unresolved until the delete is answered

    const QJsonObject del = QJsonDocument::fromJson(batch.first()).object();
    const QJsonObject response{{"server_epoch", "e1"},
                               {"results", QJsonArray{QJsonObject{{"op_id", del["op_id"]}, {"status", "applied"},
                                                                  {"rev", 2}, {"seq", 5}, {"deleted", true}}}}};
    QCOMPARE(env.repo.sync().applyPushResponse(response, 1), SyncStore::AckOutcome::Committed);
    q.exec("SELECT after_op_id, base_rev FROM sync_outbox");
    QVERIFY(q.next());
    QVERIFY(q.value(0).isNull());
    QCOMPARE(q.value(1).toInt(), 2);
    QVERIFY(env.repo.sync().prepareBatch(&batch));
    QCOMPARE(batch.size(), 1);
    QCOMPARE(QJsonDocument::fromJson(batch.first()).object()["type"].toString(), QString("restore"));
}

void TestSync::purgeWritesTombstonesWithReasons()
{
    Env env;
    QVERIFY(env.repo.sync().setLinked(true, "dev-a"));
    Task a = env.add("a"), b = env.add("b"), c = env.add("c");
    env.acknowledgeAll(1);
    QVERIFY(env.repo.setCompleted(a.id, true));
    QVERIFY(env.repo.setCompleted(b.id, true));
    env.acknowledgeAll(2);

    QList<qint64> ids;
    QVERIFY(env.repo.purgeCompleted(QDateTime::currentDateTimeUtc().addSecs(60), &ids));
    QCOMPARE(ids.size(), 2);
    QCOMPARE(env.outboxTypes(), (QStringList{"delete", "delete"}));
    QSqlQuery q(env.db.connection());
    q.exec("SELECT DISTINCT reason, base_rev FROM sync_outbox");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QString("retention"));
    QCOMPARE(q.value(1).toInt(), 2);
    QCOMPARE(env.count("sync_tombstones"), 2);

    env.acknowledgeAll(3);
    QVERIFY(env.repo.setCompleted(c.id, true));
    env.acknowledgeAll(2);
    QVERIFY(env.repo.purgeCompleted(std::nullopt, &ids));
    q.exec("SELECT reason FROM sync_outbox");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QString("clear_completed"));
    // "Immediately" deletes through deleteTask with the retention reason.
    Task d = env.add("d");
    env.acknowledgeAll(1);
    QVERIFY(env.repo.deleteTask(d.id, "retention"));
    q.exec("SELECT reason FROM sync_outbox WHERE task_uuid = '" + d.uuid + "'");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QString("retention"));
}

void TestSync::failedMutationLeavesNothingBehind()
{
    Env env;
    QVERIFY(env.repo.sync().setLinked(true, "dev-a"));
    Task t = env.add("keep");
    env.acknowledgeAll(1);

    QSqlQuery trig(env.db.connection());
    QVERIFY(trig.exec("CREATE TRIGGER fail_outbox BEFORE INSERT ON sync_outbox "
                      "BEGIN SELECT RAISE(ABORT, 'simulated failure'); END"));
    const int tasksBefore = env.count("tasks");

    Task draft;
    draft.title = "will fail";
    QVERIFY(!env.repo.addTask(draft));
    QCOMPARE(env.count("tasks"), tasksBefore);

    t.title = "changed";
    QVERIFY(!env.repo.updateTask(t));
    QVERIFY(!env.repo.setCompleted(t.id, true));
    QVERIFY(!env.repo.deleteTask(t.id));
    Task now;
    QVERIFY(env.repo.getTask(t.id, &now));
    QCOMPARE(now.title, QString("keep"));
    QVERIFY(!now.completed);
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(env.count("sync_tombstones"), 0);

    // A mutation that fails on its own merits writes no row either.
    trig.exec("DROP TRIGGER fail_outbox");
    Task ghost;
    ghost.id = 9999;
    ghost.title = "x";
    QVERIFY(!env.repo.updateTask(ghost));
    QVERIFY(!env.repo.deleteTask(9999));
    QCOMPARE(env.count("sync_outbox"), 0);
}

void TestSync::heldTaskQueuesNothing()
{
    Env env;
    QVERIFY(env.repo.sync().setLinked(true, "dev-a"));
    Task t = env.add("h");
    env.acknowledgeAll(1);
    QSqlQuery q(env.db.connection());
    QVERIFY(q.exec("UPDATE tasks SET held = 'deleted_in_cloud', held_rev = 3"));
    t.title = "edited while held";
    QVERIFY(env.repo.updateTask(t));
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(env.count("sync_base"), 1); // the held task still has its base
    QVERIFY(env.repo.deleteTask(t.id)); // "delete locally": the server already has the tombstone
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(env.count("sync_tombstones"), 0);
    QCOMPARE(env.count("sync_base"), 0); // no orphaned base
    QCOMPARE(env.count("tasks"), 0);     // and the task is gone
}

void TestSync::firstEpochIsPersisted()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    QVERIFY(store.stateValue("epoch").isEmpty());
    env.add("e");
    {
        // A missing or empty epoch is malformed: nothing is applied or remembered.
        QList<QByteArray> pending;
        QVERIFY(store.prepareBatch(&pending));
        const QJsonObject pendingOp = QJsonDocument::fromJson(pending.first()).object();
        const QJsonArray pendingResults{QJsonObject{{"op_id", pendingOp["op_id"]}, {"status", "applied"},
                                                    {"rev", 1}, {"task", pendingOp["task"]}}};
        const qint64 gen = store.stateValue("link_generation").toLongLong();
        QCOMPARE(store.applyPushResponse({{"server_epoch", ""}, {"results", pendingResults}}, gen),
                 SyncStore::AckOutcome::UnknownOutcome);
        QCOMPARE(store.applyPushResponse({{"results", pendingResults}}, gen),
                 SyncStore::AckOutcome::UnknownOutcome);
        QVERIFY(store.stateValue("epoch").isEmpty());
    }
    env.acknowledgeAll(1); // answered under epoch "e1"
    QCOMPARE(store.stateValue("epoch"), QString("e1"));
    QSqlQuery q(env.db.connection());
    QVERIFY(q.exec("SELECT epoch FROM sync_base") && q.next());
    QCOMPARE(q.value(0).toString(), QString("e1")); // bases are stamped with it

    // A later answer under another epoch is refused and leaves the stored one alone.
    Task t2 = env.add("f");
    QList<QByteArray> batch;
    QVERIFY(store.prepareBatch(&batch));
    const QJsonObject op = QJsonDocument::fromJson(batch.first()).object();
    const QJsonObject response{{"server_epoch", "e2"},
                               {"results", QJsonArray{QJsonObject{{"op_id", op["op_id"]}, {"status", "applied"},
                                                                   {"rev", 1}, {"task", op["task"]}}}}};
    QCOMPARE(store.applyPushResponse(response, store.stateValue("link_generation").toLongLong()),
             SyncStore::AckOutcome::EpochChanged);
    QCOMPARE(store.stateValue("epoch"), QString("e1"));
}

// ===========================================================================
// M1 corrective pass: regression tests for findings F1-F11
// ===========================================================================

// F1: edits made after Keep and before the restore is answered must survive it.
void TestSync::keepThenEditPreservesTheEdit()
{
    for (const bool sent : {false, true}) {
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        holdAll(env, 2);
        QVERIFY(store.holdKeep(t.uuid));
        QList<QByteArray> batch;
        if (sent) {
            QVERIFY(store.prepareBatch(&batch));
            QCOMPARE(batch.size(), 1);
        }
        t.title = "edited after keep";
        QVERIFY(env.repo.updateTask(t));
        if (sent) {
            QCOMPARE(env.outboxTypes(), (QStringList{"restore", "update"}));
            QVERIFY(!scalarOf(env, "SELECT after_op_id FROM sync_outbox WHERE type = 'update'").isNull());
        } else {
            QCOMPARE(env.outboxTypes(), QStringList{"restore"}); // folded into the unsent restore
            QVERIFY(store.prepareBatch(&batch));
            QCOMPARE(batch.size(), 1);
            QVERIFY(QString::fromUtf8(batch.first()).contains("edited after keep"));
        }
        // The server restores the task as it was sent.
        QCOMPARE(store.applyPushResponse(responseOf({okResult(opOf(batch.first()), 3, "restored")}), linkGen(env)),
                 SyncStore::AckOutcome::Committed);
        Task now;
        QVERIFY(env.repo.getTask(t.id, &now));
        QCOMPARE(now.title, QString("edited after keep"));
        QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 0);
        if (sent) {
            QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox WHERE type = 'update'").toInt(), 3);
            QVERIFY(scalarOf(env, "SELECT after_op_id FROM sync_outbox WHERE type = 'update'").isNull());
        }
    }
}

// F2: a Keep that is queued or in flight must not bring back a task the user then deleted.
void TestSync::deleteAfterKeepNeverRecreatesTheTask()
{
    {   // Keep not sent yet: both the restore and the delete vanish.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        holdAll(env, 2);
        QVERIFY(store.holdKeep(t.uuid));
        QVERIFY(env.repo.deleteTask(t.id));
        QCOMPARE(env.count("sync_outbox"), 0);
        QCOMPARE(env.count("tasks"), 0);
        QCOMPARE(env.count("sync_tombstones"), 0);
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));
        QVERIFY(batch.isEmpty());
    }
    {   // Keep in flight: the delete follows the answer, based on the restored revision.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        holdAll(env, 2);
        QVERIFY(store.holdKeep(t.uuid));
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));
        QVERIFY(env.repo.deleteTask(t.id));
        QCOMPARE(env.outboxTypes(), (QStringList{"restore", "delete"}));
        QVERIFY(scalarOf(env, "SELECT base_rev FROM sync_outbox WHERE type = 'delete'").isNull());
        QCOMPARE(store.applyPushResponse(responseOf({okResult(opOf(batch.first()), 3, "restored")}), linkGen(env)),
                 SyncStore::AckOutcome::Committed);
        QCOMPARE(env.outboxTypes(), QStringList{"delete"});
        QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox").toInt(), 3);
        QCOMPARE(env.count("tasks"), 0);
        QCOMPARE(env.count("sync_tombstones"), 1); // the delete keeps its record

        QVERIFY(store.prepareBatch(&batch));
        QCOMPARE(batch.size(), 1);
        QCOMPARE(opOf(batch.first())["type"].toString(), QString("delete"));
        QCOMPARE(store.applyPushResponse(responseOf({okResult(opOf(batch.first()), 4)}), linkGen(env)),
                 SyncStore::AckOutcome::Committed);
        QCOMPARE(env.count("sync_outbox"), 0);
        QCOMPARE(scalarOf(env, "SELECT tombstone_rev FROM sync_tombstones").toInt(), 4);
    }
    {   // "Delete locally" takes the same path.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        holdAll(env, 2);
        QVERIFY(store.holdKeep(t.uuid));
        QVERIFY(store.holdDeleteLocal(t.uuid));
        QCOMPARE(env.count("sync_outbox"), 0);
        QCOMPARE(env.count("tasks"), 0);
    }
}

// F4 (and F11 for the revisions): an acknowledgement that is not valid changes nothing.
void TestSync::acknowledgementsWithUnknownStatusChangeNothing()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    env.add("a");
    QList<QByteArray> batch;
    QVERIFY(store.prepareBatch(&batch));
    const QJsonObject op = opOf(batch.first());

    auto unchanged = [&] {
        return env.count("sync_outbox WHERE state = 'in_flight'") == 1 && env.count("sync_base") == 0
            && env.count("tasks WHERE server_rev IS NULL") == 1 && store.stateValue("epoch").isEmpty();
    };
    QVERIFY(unchanged());

    QList<QJsonObject> bad;
    auto variant = [&](const std::function<void(QJsonObject &)> &edit) {
        QJsonObject r = okResult(op, 1);
        edit(r);
        bad << r;
    };
    variant([](QJsonObject &r) { r["status"] = "bogus"; });
    variant([](QJsonObject &r) { r["status"] = "Applied"; });
    variant([](QJsonObject &r) { r["status"] = 5; });
    variant([](QJsonObject &r) { r.remove("status"); });
    variant([](QJsonObject &r) { r.remove("rev"); });
    variant([](QJsonObject &r) { r["rev"] = 0; });
    variant([](QJsonObject &r) { r["rev"] = 1.5; });
    variant([](QJsonObject &r) { r["rev"] = "2"; });
    variant([](QJsonObject &r) { r.remove("task"); });
    variant([](QJsonObject &r) { r["status"] = "rejected"; r.remove("rev"); r.remove("task"); });
    for (const QJsonObject &result : bad) {
        QCOMPARE(store.applyPushResponse(responseOf({result}), linkGen(env)), SyncStore::AckOutcome::UnknownOutcome);
        QVERIFY2(unchanged(), qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
    }
    QCOMPARE(store.applyPushResponse(responseOf({okResult(op, 1)}), linkGen(env)), SyncStore::AckOutcome::Committed);

    // Rejections that must carry a revision or the returned task.
    Env keep;
    SyncStore &ks = keep.repo.sync();
    QVERIFY(ks.setLinked(true, "dev-a"));
    Task t = keep.add("x");
    keep.acknowledgeAll(1);
    holdAll(keep, 2);
    QVERIFY(ks.holdKeep(t.uuid));
    QVERIFY(ks.prepareBatch(&batch));
    const QJsonObject restoreOp = opOf(batch.first());
    const QJsonObject returned = restoreOp["task"].toObject();
    const QList<QJsonObject> badRejections = {
        {{"op_id", restoreOp["op_id"]}, {"status", "rejected"}, {"code", "not_deleted"}, {"rev", 5}},
        {{"op_id", restoreOp["op_id"]}, {"status", "rejected"}, {"code", "not_deleted"}, {"task", returned}},
        {{"op_id", restoreOp["op_id"]}, {"status", "rejected"}, {"code", "edit_won"}, {"rev", 5}},
        {{"op_id", restoreOp["op_id"]}, {"status", "rejected"}, {"code", "already_deleted"}},
    };
    for (const QJsonObject &result : badRejections)
        QCOMPARE(ks.applyPushResponse(responseOf({result}), linkGen(keep)), SyncStore::AckOutcome::UnknownOutcome);
    QCOMPARE(keep.count("sync_outbox WHERE state = 'in_flight'"), 1);
    QCOMPARE(keep.count("tasks WHERE held IS NOT NULL"), 1);
}

// F3: not_deleted adopts the returned task as the base but never overwrites a different local one unseen.
void TestSync::notDeletedReconcilesInsteadOfOverwriting()
{
    for (const bool serverChangedTitle : {true, false}) {
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        holdAll(env, 2);
        t.title = "mine";
        QVERIFY(env.repo.updateTask(t)); // held: local only
        QVERIFY(store.holdKeep(t.uuid));
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));

        QJsonObject theirs = baseSnapshot(env, t.uuid); // another device restored the task meanwhile
        if (serverChangedTitle)
            theirs["title"] = "theirs";
        else
            theirs["priority"] = 2;
        const QJsonObject result{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"},
                                 {"code", "not_deleted"}, {"rev", 5}, {"task", theirs}};
        QCOMPARE(store.applyPushResponse(responseOf({result}), linkGen(env)), SyncStore::AckOutcome::Committed);

        Task now;
        QVERIFY(env.repo.getTask(t.id, &now));
        QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 0);
        QCOMPARE(scalarOf(env, "SELECT rev FROM sync_base").toInt(), 5);
        if (serverChangedTitle) {
            QCOMPARE(now.title, QString("theirs")); // both changed: the cloud value stays...
            QCOMPARE(env.count("sync_outbox"), 0);
            QCOMPARE(env.count("sync_conflicts WHERE kind = 'reconcile' AND field_group = 'title' AND state = 'open'"), 1);
            QCOMPARE(QJsonDocument::fromJson(scalarOf(env, "SELECT mine_value FROM sync_conflicts").toString().toUtf8())
                         .array().at(0).toString(),
                     QString("mine")); // ...and the local text is preserved
        } else {
            QCOMPARE(now.title, QString("mine")); // only this device changed it: sent
            QCOMPARE(now.priority, 2);            // the cloud changed another group: taken
            QCOMPARE(env.outboxTypes(), QStringList{"update"});
            QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox").toInt(), 5);
            QCOMPARE(env.count("sync_conflicts"), 0);
        }
    }
}

// F6: the strict restore-row rule also applies when an acknowledgement brings newer cloud data.
void TestSync::restoreRowIsDroppedWhenAnAcknowledgementBringsNewCloudData()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("x");
    env.acknowledgeAll(1);
    QVERIFY(store.storeConflict({{"id", "c1"}, {"source", "local"}, {"kind", "reconcile"}, {"task_id", t.uuid},
                                 {"field_group", "title"}, {"mine_value", "mine"}, {"theirs_value", "x"},
                                 {"server_rev_seen", 1}}));
    t.listName = "Work";
    QVERIFY(env.repo.updateTask(t));
    QList<QByteArray> batch;
    QVERIFY(store.prepareBatch(&batch)); // the list change is in flight
    SyncStore::ConflictResolution res;
    QVERIFY(store.resolveConflict("c1", "restore", "2026-11-01T00:00:00.000Z", &res));
    QCOMPARE(res.result, QString("queued"));
    QCOMPARE(env.outboxTypes(), (QStringList{"update", "update"}));

    // The answer includes a newer title from another device.
    QJsonObject merged = opOf(batch.first())["task"].toObject();
    merged["title"] = "cloud-new";
    QJsonObject result = okResult(opOf(batch.first()), 2, "merged");
    result["task"] = merged;
    QCOMPARE(store.applyPushResponse(responseOf({result}), linkGen(env)), SyncStore::AckOutcome::Committed);

    QCOMPARE(env.count("sync_outbox"), 0); // the restore was not sent
    Task now;
    QVERIFY(env.repo.getTask(t.id, &now));
    QCOMPARE(now.title, QString("cloud-new")); // newer cloud data is not overwritten
    QCOMPARE(scalarOf(env, "SELECT state FROM sync_conflicts WHERE id = 'c1'").toString(), QString("open"));
    QCOMPARE(scalarOf(env, "SELECT server_rev_seen FROM sync_conflicts WHERE id = 'c1'").toInt(), 2);
    QCOMPARE(QJsonDocument::fromJson(scalarOf(env, "SELECT theirs_value FROM sync_conflicts WHERE id = 'c1'")
                                         .toString().toUtf8()).array().at(0).toString(),
             QString("cloud-new"));
    // The preserved local value is untouched.
    QCOMPARE(QJsonDocument::fromJson(scalarOf(env, "SELECT mine_value FROM sync_conflicts WHERE id = 'c1'")
                                         .toString().toUtf8()).array().at(0).toString(),
             QString("mine"));
}

// F7: revisions never move backwards.
void TestSync::revisionsOnlyMoveForward()
{
    {   // A tombstone that is not newer than the live copy is stale.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(3);
        QVERIFY(store.applyPullPage({pullTombstone(t.uuid, 2)}, {}, QVariant()));
        QCOMPARE(env.count("tasks"), 1);
        QVERIFY(store.applyPullPage({pullTombstone(t.uuid, 4)}, {}, QVariant()));
        QCOMPARE(env.count("tasks"), 0);
    }
    {   // The recorded tombstone of a held task only grows.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        holdAll(env, 5);
        QVERIFY(store.applyPullPage({pullTombstone(t.uuid, 3)}, {}, QVariant()));
        QCOMPARE(scalarOf(env, "SELECT held_rev FROM tasks").toInt(), 5);
        QVERIFY(store.applyPullPage({pullTombstone(t.uuid, 7)}, {}, QVariant()));
        QCOMPARE(scalarOf(env, "SELECT held_rev FROM tasks").toInt(), 7);
        // A live copy that predates the recorded deletion does not end the hold.
        QJsonObject copy = baseSnapshot(env, t.uuid);
        copy["title"] = "older";
        QVERIFY(store.applyPullPage({pullTask(t.uuid, 6, copy)}, {}, QVariant()));
        QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 1);
    }
    {   // A removed task is not re-created by a copy older than its known tombstone.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        const QJsonObject copy = baseSnapshot(env, t.uuid);
        QVERIFY(env.repo.deleteTask(t.id));
        env.acknowledgeAll(2); // tombstone revision 2
        QCOMPARE(env.count("tasks"), 0);
        QVERIFY(store.applyPullPage({pullTask(t.uuid, 1, copy)}, {}, QVariant()));
        QCOMPARE(env.count("tasks"), 0);
        QVERIFY(store.applyPullPage({pullTask(t.uuid, 3, copy)}, {}, QVariant())); // resurrected elsewhere
        QCOMPARE(env.count("tasks"), 1);
    }
    {   // Setting revisions.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        const QJsonObject newer{{"kind", "setting"}, {"key", "completed_cleanup_policy"}, {"rev", 5}, {"seq", 5},
                                {"value", "days:3"}};
        QJsonObject older = newer;
        older["rev"] = 3;
        older["value"] = "never";
        QVERIFY(store.applyPullPage({newer}, {}, QVariant()));
        QVERIFY(store.applyPullPage({older}, {}, QVariant()));
        QCOMPARE(store.stateValue("setting.completed_cleanup_policy"), QString("days:3"));
        QCOMPARE(store.stateValue("setting_rev.completed_cleanup_policy"), QString("5"));
    }
}

// F8: an edit made while unlinked has no outbox row; a later pull must not replace it unseen.
void TestSync::editsMadeWhileUnlinkedSurviveAPull()
{
    for (const bool serverChangedTitle : {false, true}) {
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        QVERIFY(store.setLinked(false));
        t.title = "local while unlinked";
        QVERIFY(env.repo.updateTask(t));
        QCOMPARE(env.count("sync_outbox"), 0);
        QVERIFY(!store.applyPullPage({}, {}, QVariant())); // nothing is applied while unlinked
        QVERIFY(store.setLinked(true, "dev-a"));

        QJsonObject copy = baseSnapshot(env, t.uuid);
        copy["priority"] = 2;
        if (serverChangedTitle)
            copy["title"] = "cloud title";
        QVERIFY(store.applyPullPage({pullTask(t.uuid, 2, copy)}, {}, QVariant()));
        Task now;
        QVERIFY(env.repo.getTask(t.id, &now));
        QCOMPARE(now.priority, 2);
        if (serverChangedTitle) {
            QCOMPARE(now.title, QString("cloud title"));
            QCOMPARE(env.count("sync_conflicts WHERE kind = 'reconcile' AND field_group = 'title'"), 1);
            QCOMPARE(QJsonDocument::fromJson(scalarOf(env, "SELECT mine_value FROM sync_conflicts").toString().toUtf8())
                         .array().at(0).toString(),
                     QString("local while unlinked"));
            QCOMPARE(env.count("sync_outbox"), 0);
        } else {
            QCOMPARE(now.title, QString("local while unlinked"));
            QCOMPARE(env.outboxTypes(), QStringList{"update"});
            QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox").toInt(), 2);
            QCOMPARE(scalarOf(env, "SELECT changed FROM sync_outbox").toString(), QString("[\"title\"]"));
        }
    }
    {   // A task created while unlinked that the server also knows (same id): nothing is lost.
        Env env;
        SyncStore &store = env.repo.sync();
        Task t = env.add("created unlinked");
        QVERIFY(store.setLinked(true, "dev-a"));
        QJsonObject copy = defaultTask(t.uuid);
        copy["title"] = "server title";
        QVERIFY(store.applyPullPage({pullTask(t.uuid, 4, copy)}, {}, QVariant()));
        Task now;
        QVERIFY(env.repo.getTask(t.id, &now));
        QCOMPARE(now.title, QString("server title"));
        QCOMPARE(env.count("sync_conflicts WHERE field_group = 'title'"), 1);
    }
}

// F9 / F10 (not_synced and Keep): edge cases around dependent and refused operations.
void TestSync::notSyncedAndKeepEdgeCases()
{
    {   // An update that changes nothing is not a deliberate edit: the mark stays.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        QSqlQuery q(env.db.connection());
        QVERIFY(q.exec("UPDATE tasks SET not_synced = 'validation_failed'"));
        Task same;
        QVERIFY(env.repo.getTask(t.id, &same));
        QVERIFY(env.repo.updateTask(same));
        QCOMPARE(env.count("tasks WHERE not_synced IS NOT NULL"), 1);
        QCOMPARE(env.count("sync_outbox"), 0);
        same.title = "fixed";
        QVERIFY(env.repo.updateTask(same));
        QCOMPARE(env.count("tasks WHERE not_synced IS NOT NULL"), 0);
        QCOMPARE(env.outboxTypes(), QStringList{"update"});
    }
    {   // A second Keep never queues a second restore.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        holdAll(env, 2);
        QVERIFY(store.holdKeep(t.uuid));
        QVERIFY(store.holdKeep(t.uuid));
        QCOMPARE(env.outboxTypes(), QStringList{"restore"});
    }
    {   // already_exists is reconciled by 9.2: the server copy stays, local text is preserved.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("mine");
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));
        QJsonObject theirs = opOf(batch.first())["task"].toObject();
        theirs["title"] = "theirs";
        const QJsonObject result{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"},
                                 {"code", "already_exists"}, {"rev", 3}, {"task", theirs}};
        QCOMPARE(store.applyPushResponse(responseOf({result}), linkGen(env)), SyncStore::AckOutcome::Committed);
        Task now;
        QVERIFY(env.repo.getTask(t.id, &now));
        QCOMPARE(now.title, QString("theirs"));
        QCOMPARE(env.count("sync_conflicts WHERE field_group = 'title' AND state = 'open'"), 1);
        QCOMPARE(env.count("tasks WHERE not_synced IS NOT NULL"), 0);
        QCOMPARE(scalarOf(env, "SELECT rev FROM sync_base").toInt(), 3);
        QCOMPARE(env.count("sync_outbox"), 0);
    }
    {   // A restore refused with invalid_base_rev leaves the task held, edits intact: ask again.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        holdAll(env, 2);
        QVERIFY(store.holdKeep(t.uuid));
        t.title = "kept edit";
        QVERIFY(env.repo.updateTask(t));
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));
        const QJsonObject result{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"},
                                 {"code", "invalid_base_rev"}};
        QCOMPARE(store.applyPushResponse(responseOf({result}), linkGen(env)), SyncStore::AckOutcome::Committed);
        QCOMPARE(env.count("sync_outbox"), 0);
        QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 1);
        Task now;
        QVERIFY(env.repo.getTask(t.id, &now));
        QCOMPARE(now.title, QString("kept edit"));
    }
}

// F10: a record with a restore queued or in flight is not resolved a second time.
void TestSync::conflictRecordsAreGuardedWhileARestoreIsQueued()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("x");
    env.acknowledgeAll(1);
    QVERIFY(store.storeConflict({{"id", "c1"}, {"source", "local"}, {"kind", "reconcile"}, {"task_id", t.uuid},
                                 {"field_group", "title"}, {"mine_value", "mine"}, {"theirs_value", "x"},
                                 {"server_rev_seen", 1}}));
    const QString now = "2026-11-01T00:00:00.000Z";
    SyncStore::ConflictResolution res;
    QVERIFY(store.resolveConflict("c1", "restore", now, &res));
    QCOMPARE(res.result, QString("queued"));
    QCOMPARE(env.count("sync_outbox"), 1);

    for (const QString &action : {QStringLiteral("restore"), QStringLiteral("restore_confirmed"),
                                  QStringLiteral("keep"), QStringLiteral("delete")}) {
        QVERIFY(store.resolveConflict("c1", action, now, &res));
        QCOMPARE(res.result, QString("unavailable"));
        QCOMPARE(env.count("sync_outbox"), 1);
        QCOMPARE(env.count("sync_conflicts WHERE id = 'c1' AND state = 'restored'"), 1);
    }

    // After the row is answered with a refusal the record is open again and can be resolved.
    QList<QByteArray> batch;
    QVERIFY(store.prepareBatch(&batch));
    const QJsonObject refusal{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"},
                              {"code", "validation_failed"}};
    QCOMPARE(store.applyPushResponse(responseOf({refusal}), linkGen(env)), SyncStore::AckOutcome::Committed);
    QCOMPARE(env.count("sync_conflicts WHERE id = 'c1' AND state = 'open'"), 1);
    QVERIFY(store.resolveConflict("c1", "keep", now, &res));
    QCOMPARE(res.result, QString("kept_cloud"));
    // Keeping again does not move the resolution time that retention counts from.
    QVERIFY(store.resolveConflict("c1", "keep", "2026-12-24T00:00:00.000Z", &res));
    QCOMPARE(res.result, QString("kept_cloud"));
    QCOMPARE(scalarOf(env, "SELECT resolved_at FROM sync_conflicts WHERE id = 'c1'").toString(), now);
}

// F11: tombstones never remove content the server did not give us or the user has not sent.
void TestSync::tombstonesNeverRemoveUnsentContent()
{
    {   // Created while unlinked, never on the server: hold instead of deleting.
        Env env;
        SyncStore &store = env.repo.sync();
        Task t = env.add("only here");
        QVERIFY(store.setLinked(true, "dev-a"));
        QVERIFY(store.applyPullPage({pullTombstone(t.uuid, 2)}, {}, QVariant()));
        QCOMPARE(env.count("tasks"), 1);
        QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 1);
    }
    {   // Reconciling a tombstone holds the task and cancels what was queued for it (11.6, 11.8).
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        t.title = "unsent edit";
        QVERIFY(env.repo.updateTask(t));
        QCOMPARE(env.count("sync_outbox"), 1);
        QVERIFY(store.reconcileTask(pullTombstone(t.uuid, 5)));
        QCOMPARE(env.count("sync_outbox"), 0);
        QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 1);
        Task now;
        QVERIFY(env.repo.getTask(t.id, &now));
        QCOMPARE(now.title, QString("unsent edit")); // the content stays for the user to decide
    }
}

// ===========================================================================
// M1.2 regression tests
// ===========================================================================

namespace {

// Held task, Keep sent (restore in flight), an edit queued behind it, the task deleted and the
// delete undone before the answer. Returns the restore's op id.
QString keepDeleteUndo(Env &env, Task *taskOut, QByteArray *restorePayload)
{
    SyncStore &store = env.repo.sync();
    if (!store.setLinked(true, "dev-a"))
        qFatal("link");
    Task t = env.add("x");
    env.acknowledgeAll(1);
    holdAll(env, 2);
    if (!store.holdKeep(t.uuid))
        qFatal("keep");
    QList<QByteArray> batch;
    if (!store.prepareBatch(&batch))
        qFatal("batch");
    *restorePayload = batch.first();
    t.title = "edited behind the restore";
    if (!env.repo.updateTask(t))
        qFatal("edit");
    Task current;
    if (!env.repo.getTask(t.id, &current))
        qFatal("get");
    if (!env.repo.deleteTask(t.id))
        qFatal("delete");
    if (!env.repo.restoreTask(current, nullptr))
        qFatal("undo");
    *taskOut = current;
    return opOf(batch.first())["op_id"].toString();
}

} // namespace

// An Undo while the Keep's restore is in flight puts back the held state, the base, the revision
// and the edits, with the same restore in flight; no operation is deleted or renumbered.
void TestSync::undoDuringInFlightRestoreKeepsTheHold()
{
    for (const bool accepted : {true, false}) {
        Env env;
        SyncStore &store = env.repo.sync();
        Task t;
        QByteArray restorePayload;
        const QString restoreId = keepDeleteUndo(env, &t, &restorePayload);

        QCOMPARE(env.count("tasks"), 1);
        QCOMPARE(env.count("tasks WHERE held = 'deleted_in_cloud' AND held_rev = 2 AND server_rev = 1"), 1);
        QCOMPARE(env.count("sync_base WHERE rev = 1"), 1);
        QCOMPARE(env.count("sync_tombstones"), 0);
        QCOMPARE(env.outboxTypes(), (QStringList{"restore", "update"}));
        QCOMPARE(scalarOf(env, "SELECT op_id FROM sync_outbox WHERE type = 'restore'").toString(), restoreId);
        QCOMPARE(scalarOf(env, "SELECT state FROM sync_outbox WHERE type = 'restore'").toString(), QString("in_flight"));
        QCOMPARE(scalarOf(env, "SELECT sent_payload FROM sync_outbox WHERE type = 'restore'").toString().toUtf8(),
                 restorePayload);
        QCOMPARE(scalarOf(env, "SELECT after_op_id FROM sync_outbox WHERE type = 'update'").toString(), restoreId);
        QVERIFY(scalarOf(env, "SELECT base_rev FROM sync_outbox WHERE type = 'update'").isNull());
        Task now;
        QVERIFY(env.repo.getTask(t.id, &now));
        QCOMPARE(now.title, QString("edited behind the restore"));
        // Only the in-flight restore is sent again, byte-identical.
        QList<QByteArray> again;
        QVERIFY(store.prepareBatch(&again));
        QCOMPARE(again, QList<QByteArray>{restorePayload});

        if (accepted) {
            QCOMPARE(store.applyPushResponse(responseOf({okResult(opOf(restorePayload), 3, "restored")}), linkGen(env)),
                     SyncStore::AckOutcome::Committed);
            QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 0);
            QCOMPARE(env.count("sync_base WHERE rev = 3"), 1);
            QCOMPARE(env.outboxTypes(), QStringList{"update"});
            QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox").toInt(), 3);
            QVERIFY(scalarOf(env, "SELECT after_op_id FROM sync_outbox").isNull());
            QVERIFY(env.repo.getTask(t.id, &now));
            QCOMPARE(now.title, QString("edited behind the restore"));
        } else {
            const QJsonObject rejection{{"op_id", restoreId}, {"status", "rejected"}, {"code", "invalid_base_rev"}};
            QCOMPARE(store.applyPushResponse(responseOf({rejection}), linkGen(env)), SyncStore::AckOutcome::Committed);
            // Void Keep: nothing is retried, the task stays held with its content, the user decides again.
            QCOMPARE(env.count("sync_outbox"), 0);
            QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 1);
            QVERIFY(scalarOf(env, "SELECT held_rev FROM tasks").isNull());
            QVERIFY(env.repo.getTask(t.id, &now));
            QCOMPARE(now.title, QString("edited behind the restore"));
            QVERIFY(!store.holdKeep(t.uuid)); // refused until a pull says what the server holds
            bool ask = false, reconcile = false;
            QVERIFY(store.holdRestoreRejected(t.uuid, true, 5, &ask, &reconcile));
            QVERIFY(ask && !reconcile);
            QVERIFY(store.holdKeep(t.uuid));
            QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox").toInt(), 5);
            QVERIFY(scalarOf(env, "SELECT op_id FROM sync_outbox").toString() != restoreId);
        }
    }
}

// F5: a rejected operation is retried once under a new op_id (the server stored the rejection),
// then reconciled; it can never loop.
void TestSync::invalidBaseRevRetriesOnceUnderANewOpId()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("x");
    env.acknowledgeAll(1);
    t.title = "first";
    QVERIFY(env.repo.updateTask(t));
    QList<QByteArray> batch;
    QVERIFY(store.prepareBatch(&batch));
    const QString first = opOf(batch.first())["op_id"].toString();
    t.priority = 2;
    QVERIFY(env.repo.updateTask(t)); // waits behind the one in flight
    const qint64 seq = scalarOf(env, "SELECT seq FROM sync_outbox WHERE op_id = '" + first + "'").toLongLong();

    auto reject = [&](const QString &opId) {
        const QJsonObject r{{"op_id", opId}, {"status", "rejected"}, {"code", "invalid_base_rev"}};
        return store.applyPushResponse(responseOf({r}), linkGen(env)) == SyncStore::AckOutcome::Committed;
    };
    QVERIFY(reject(first));
    const QString second = scalarOf(env, "SELECT op_id FROM sync_outbox ORDER BY seq LIMIT 1").toString();
    QVERIFY(second != first);
    QCOMPARE(env.count("sync_outbox"), 2);
    QCOMPARE(scalarOf(env, "SELECT seq FROM sync_outbox WHERE op_id = '" + second + "'").toLongLong(), seq);
    QCOMPARE(scalarOf(env, "SELECT attempts FROM sync_outbox WHERE op_id = '" + second + "'").toInt(), 1);
    QCOMPARE(env.count("sync_outbox WHERE after_op_id = '" + second + "'"), 1); // the dependent follows
    QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox WHERE op_id = '" + second + "'").toInt(), 1);
    QCOMPARE(scalarOf(env, "SELECT rev FROM sync_base").toInt(), 1); // the base is untouched

    QVERIFY(store.prepareBatch(&batch));
    QCOMPARE(batch.size(), 1);
    QCOMPARE(opOf(batch.first())["op_id"].toString(), second);

    QVERIFY(reject(second)); // rejected again: no third attempt
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(scalarOf(env, "SELECT not_synced FROM tasks").toString(), QString("needs_reconcile"));
    QCOMPARE(scalarOf(env, "SELECT trusted FROM sync_base").toInt(), 0);
    Task now;
    QVERIFY(env.repo.getTask(t.id, &now));
    QCOMPARE(now.title, QString("first")); // local content untouched
    QCOMPARE(now.priority, 2);
    QVERIFY(store.prepareBatch(&batch));
    QVERIFY(batch.isEmpty());
}

// F5: a restore refused with invalid_base_rev is not retried when the task is gone either.
void TestSync::restoreRejectedWithInvalidBaseRevIsNeverRetried()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("x");
    env.acknowledgeAll(1);
    holdAll(env, 2);
    QVERIFY(store.holdKeep(t.uuid));
    QList<QByteArray> batch;
    QVERIFY(store.prepareBatch(&batch));
    const QString restoreId = opOf(batch.first())["op_id"].toString();
    QVERIFY(env.repo.deleteTask(t.id)); // the delete waits behind the restore
    const QJsonObject r{{"op_id", restoreId}, {"status", "rejected"}, {"code", "invalid_base_rev"}};
    QCOMPARE(store.applyPushResponse(responseOf({r}), linkGen(env)), SyncStore::AckOutcome::Committed);
    // The Keep is void; the user's delete stays, based on the last copy this device saw.
    QCOMPARE(env.outboxTypes(), QStringList{"delete"});
    QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox").toInt(), 1);
    QVERIFY(scalarOf(env, "SELECT after_op_id FROM sync_outbox").isNull());
    QVERIFY(store.prepareBatch(&batch));
    QCOMPARE(batch.size(), 1);
    QCOMPARE(opOf(batch.first())["type"].toString(), QString("delete"));
}

// ===========================================================================
// M1.3 regression tests: not_synced paths (F9), conflict resolution (F10), review items (F12)
// ===========================================================================

namespace {

// A task that was acknowledged at revision 1, edited to `title`, whose update was then
// refused for content reasons (not_synced = validation_failed, base still "x").
Task refusedEdit(Env &env, const QString &title)
{
    SyncStore &store = env.repo.sync();
    if (!store.setLinked(true, "dev-a"))
        qFatal("link");
    Task t = env.add("x");
    env.acknowledgeAll(1);
    t.title = title;
    if (!env.repo.updateTask(t))
        qFatal("edit");
    QList<QByteArray> batch;
    if (!store.prepareBatch(&batch))
        qFatal("batch");
    const QJsonObject refusal{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"},
                              {"code", "validation_failed"}};
    if (store.applyPushResponse(responseOf({refusal}), linkGen(env)) != SyncStore::AckOutcome::Committed)
        qFatal("refuse");
    return t;
}

} // namespace

// F9: a refused create has no base. The next deliberate edit sends a create, not an update.
void TestSync::notSyncedWithoutABaseQueuesACreate()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("never accepted");
    QList<QByteArray> batch;
    QVERIFY(store.prepareBatch(&batch));
    const QJsonObject refusal{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"},
                              {"code", "validation_failed"}};
    QCOMPARE(store.applyPushResponse(responseOf({refusal}), linkGen(env)), SyncStore::AckOutcome::Committed);
    QCOMPARE(env.count("sync_base"), 0);
    QCOMPARE(scalarOf(env, "SELECT not_synced FROM tasks").toString(), QString("validation_failed"));
    QVERIFY(scalarOf(env, "SELECT server_rev FROM tasks").isNull());
    QCOMPARE(env.count("sync_outbox"), 0);

    Task same;
    QVERIFY(env.repo.getTask(t.id, &same));
    QVERIFY(env.repo.updateTask(same)); // nothing changed: no operation, the mark stays
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(env.count("tasks WHERE not_synced IS NOT NULL"), 1);

    same.title = "fixed";
    QVERIFY(env.repo.updateTask(same));
    QCOMPARE(env.outboxTypes(), QStringList{"create"});
    QCOMPARE(env.count("tasks WHERE not_synced IS NOT NULL"), 0);
    QVERIFY(store.prepareBatch(&batch));
    QCOMPARE(opOf(batch.first())["task"].toObject()["title"].toString(), QString("fixed"));
}

// F9: an edit that brings a refused task back to its base leaves nothing unsynced.
void TestSync::notSyncedEndsWhenTheTaskIsBackAtItsBase()
{
    Env env;
    Task t = refusedEdit(env, "changed");
    QCOMPARE(scalarOf(env, "SELECT not_synced FROM tasks").toString(), QString("validation_failed"));
    QCOMPARE(env.count("sync_outbox"), 0);
    Task now;
    QVERIFY(env.repo.getTask(t.id, &now));
    now.title = "x"; // the base value
    QVERIFY(env.repo.updateTask(now));
    QCOMPARE(env.count("tasks WHERE not_synced IS NOT NULL"), 0);
    QCOMPARE(env.count("sync_outbox"), 0); // equal to the server's copy: nothing to send
}

// F9: delete and undo of a refused task keep its content represented.
void TestSync::notSyncedSurvivesDeleteAndUndo()
{
    {   // Undo of a delete that was never sent: the repository cannot re-insert the mark, so the
        // store flags the task again because its content differs from the base.
        Env env;
        Task t = refusedEdit(env, "changed");
        Task snapshot;
        QVERIFY(env.repo.getTask(t.id, &snapshot));
        QVERIFY(env.repo.deleteTask(t.id));
        QCOMPARE(env.outboxTypes(), QStringList{"delete"});
        QVERIFY(env.repo.restoreTask(snapshot, nullptr));
        QCOMPARE(env.count("sync_outbox"), 0);
        QCOMPARE(env.count("sync_tombstones"), 0);
        QCOMPARE(env.count("tasks"), 1);
        QCOMPARE(scalarOf(env, "SELECT not_synced FROM tasks").toString(), QString("needs_reconcile"));
        QCOMPARE(scalarOf(env, "SELECT server_rev FROM tasks").toInt(), 1);
        Task back;
        QVERIFY(env.repo.getTask(t.id, &back));
        QCOMPARE(back.title, QString("changed"));
    }
    {   // Undo of an unchanged task adds no mark.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        Task snapshot;
        QVERIFY(env.repo.getTask(t.id, &snapshot));
        QVERIFY(env.repo.deleteTask(t.id));
        QVERIFY(env.repo.restoreTask(snapshot, nullptr));
        QCOMPARE(env.count("tasks WHERE not_synced IS NOT NULL"), 0);
        QCOMPARE(env.count("sync_outbox"), 0);
    }
    {   // Deleting a refused task sends the delete, based on the revision the server holds.
        Env env;
        Task t = refusedEdit(env, "changed");
        QVERIFY(env.repo.deleteTask(t.id));
        QCOMPARE(env.outboxTypes(), QStringList{"delete"});
        QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox").toInt(), 1);
        QCOMPARE(env.count("tasks"), 0);
    }
}

// F9 / F12 (trusted): a distrusted revision is never used as a base_rev; a pull restores trust.
void TestSync::distrustedBaseIsNeverUsedAsABaseRev()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("x");
    env.acknowledgeAll(1);
    QSqlQuery q(env.db.connection());
    QVERIFY(q.exec("UPDATE sync_base SET trusted = 0"));

    t.title = "typed while distrusted";
    QVERIFY(env.repo.updateTask(t));
    QCOMPARE(env.count("sync_outbox"), 0); // not sent against a revision nobody trusts
    QCOMPARE(scalarOf(env, "SELECT not_synced FROM tasks").toString(), QString("needs_reconcile"));
    Task now;
    QVERIFY(env.repo.getTask(t.id, &now));
    QCOMPARE(now.title, QString("typed while distrusted")); // the content stays local
    t.priority = 2;
    QVERIFY(env.repo.updateTask(t));
    QCOMPARE(env.count("sync_outbox"), 0);

    // A pull brings a server copy: the base is trusted again and the local edit is reconciled
    // (carried, since the server did not change that group).
    QJsonObject copy = baseSnapshot(env, t.uuid);
    copy["notes"] = "from cloud";
    QVERIFY(store.applyPullPage({pullTask(t.uuid, 2, copy)}, {}, QVariant()));
    QCOMPARE(scalarOf(env, "SELECT trusted FROM sync_base").toInt(), 1);
    QVERIFY(env.repo.getTask(t.id, &now));
    QCOMPARE(now.title, QString("typed while distrusted"));
    QCOMPARE(now.notes, QString("from cloud"));
    QCOMPARE(env.outboxTypes(), QStringList{"update"});
    QCOMPARE(scalarOf(env, "SELECT base_rev FROM sync_outbox").toInt(), 2);
}

// F9 / F12 (trusted): the second invalid_base_rev distrusts the base; later edits stay local.
void TestSync::repeatedInvalidBaseRevDistrustsTheBase()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("x");
    env.acknowledgeAll(1);
    t.title = "first";
    QVERIFY(env.repo.updateTask(t));
    for (int attempt = 0; attempt < 2; ++attempt) {
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));
        const QJsonObject r{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"},
                            {"code", "invalid_base_rev"}};
        QCOMPARE(store.applyPushResponse(responseOf({r}), linkGen(env)), SyncStore::AckOutcome::Committed);
    }
    QCOMPARE(scalarOf(env, "SELECT trusted FROM sync_base").toInt(), 0);
    QCOMPARE(env.count("sync_outbox"), 0);
    t.priority = 2;
    QVERIFY(env.repo.updateTask(t));
    QCOMPARE(env.count("sync_outbox"), 0);
    QCOMPARE(scalarOf(env, "SELECT not_synced FROM tasks").toString(), QString("needs_reconcile"));
    Task now;
    QVERIFY(env.repo.getTask(t.id, &now));
    QCOMPARE(now.priority, 2);
    QCOMPARE(now.title, QString("first"));
}

// F10: a restore in flight owns its record; the original values stay recoverable throughout.
void TestSync::conflictRecordIsGuardedWhileTheRestoreIsInFlight()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("x");
    env.acknowledgeAll(1);
    QVERIFY(store.storeConflict({{"id", "c1"}, {"source", "local"}, {"kind", "reconcile"}, {"task_id", t.uuid},
                                 {"field_group", "title"}, {"mine_value", "mine"}, {"theirs_value", "x"},
                                 {"server_rev_seen", 1}}));
    const QString now = "2026-11-01T00:00:00.000Z";
    SyncStore::ConflictResolution res;
    QVERIFY(store.resolveConflict("c1", "restore", now, &res));
    QCOMPARE(res.result, QString("queued"));
    QList<QByteArray> batch;
    QVERIFY(store.prepareBatch(&batch)); // now in flight
    QCOMPARE(env.count("sync_outbox WHERE state = 'in_flight'"), 1);
    const QString opId = opOf(batch.first())["op_id"].toString();

    for (const QString &action : {QStringLiteral("keep"), QStringLiteral("delete"), QStringLiteral("restore"),
                                  QStringLiteral("restore_confirmed")}) {
        QVERIFY(store.resolveConflict("c1", action, now, &res));
        QCOMPARE(res.result, QString("unavailable"));
        QCOMPARE(env.count("sync_conflicts WHERE id = 'c1' AND state = 'restored'"), 1);
        QCOMPARE(scalarOf(env, "SELECT restore_op_id FROM sync_conflicts").toString(), opId);
        QCOMPARE(env.count("sync_outbox WHERE state = 'in_flight'"), 1);
    }
    // The record is not removed by the retention clean-up while the restore is on its way.
    QVERIFY(store.cleanupConflicts("2027-06-01T00:00:00.000Z"));
    QCOMPARE(env.count("sync_conflicts"), 1);

    // The server refuses: the record is open again and still holds the text.
    const QJsonObject refusal{{"op_id", opId}, {"status", "rejected"}, {"code", "validation_failed"}};
    QCOMPARE(store.applyPushResponse(responseOf({refusal}), linkGen(env)), SyncStore::AckOutcome::Committed);
    QCOMPARE(env.count("sync_conflicts WHERE id = 'c1' AND state = 'open' AND restore_op_id IS NULL"), 1);
    QCOMPARE(QJsonDocument::fromJson(scalarOf(env, "SELECT mine_value FROM sync_conflicts").toString().toUtf8())
                 .array().at(0).toString(),
             QString("mine"));
}

// F10: the record names its restore row even when the row is a create (task unknown to the server).
void TestSync::conflictRecordIsGuardedByItsRestoreOpId()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("x");
    const QString createOp = scalarOf(env, "SELECT op_id FROM sync_outbox").toString();
    QVERIFY(store.storeConflict({{"id", "c2"}, {"source", "local"}, {"kind", "reconcile"}, {"task_id", t.uuid},
                                 {"field_group", "title"}, {"mine_value", "mine"}, {"theirs_value", "x"},
                                 {"state", "restored"}, {"restore_op_id", createOp},
                                 {"resolved_at", "2026-01-01T00:00:00.000Z"}}));
    SyncStore::ConflictResolution res;
    for (const QString &action : {QStringLiteral("delete"), QStringLiteral("keep"), QStringLiteral("restore")}) {
        QVERIFY(store.resolveConflict("c2", action, "2026-11-01T00:00:00.000Z", &res));
        QCOMPARE(res.result, QString("unavailable"));
    }
    QCOMPARE(env.count("sync_conflicts"), 1);
    QVERIFY(store.cleanupConflicts("2027-06-01T00:00:00.000Z")); // old, but its row is still queued
    QCOMPARE(env.count("sync_conflicts"), 1);
    env.acknowledgeAll(1); // the create is answered: the record is finished
    QVERIFY(store.resolveConflict("c2", "delete", "2026-11-01T00:00:00.000Z", &res));
    QCOMPARE(res.result, QString("deleted"));
}

// F12: a rejected unknown_task leaves no tombstone record and no base behind.
void TestSync::unknownTaskLeavesNoTombstoneOrBase()
{
    {   // The delete is refused: the server never had the task.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        QVERIFY(env.repo.deleteTask(t.id));
        QCOMPARE(env.count("sync_tombstones"), 1);
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));
        const QJsonObject r{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"}, {"code", "unknown_task"}};
        QCOMPARE(store.applyPushResponse(responseOf({r}), linkGen(env)), SyncStore::AckOutcome::Committed);
        QCOMPARE(env.count("sync_outbox"), 0);
        QCOMPARE(env.count("sync_tombstones"), 0);
        QCOMPARE(env.count("sync_base"), 0);
        QCOMPARE(env.count("tasks"), 0);
    }
    {   // The delete was undone while in flight and then refused: the task is created again.
        Env env;
        SyncStore &store = env.repo.sync();
        QVERIFY(store.setLinked(true, "dev-a"));
        Task t = env.add("x");
        env.acknowledgeAll(1);
        Task snapshot;
        QVERIFY(env.repo.getTask(t.id, &snapshot));
        QVERIFY(env.repo.deleteTask(t.id));
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));
        QVERIFY(env.repo.restoreTask(snapshot, nullptr));
        const QJsonObject r{{"op_id", opOf(batch.first())["op_id"]}, {"status", "rejected"}, {"code", "unknown_task"}};
        QCOMPARE(store.applyPushResponse(responseOf({r}), linkGen(env)), SyncStore::AckOutcome::Committed);
        QCOMPARE(env.outboxTypes(), QStringList{"create"});
        QCOMPARE(env.count("sync_tombstones"), 0);
        QCOMPARE(env.count("tasks"), 1);
        QCOMPARE(scalarOf(env, "SELECT trusted FROM sync_base").toInt(), 0);
    }
}

// F12: old tombstone records and bases nothing refers to are collected; the rest stay.
void TestSync::oldTombstonesAreCollected()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task a = env.add("a");
    Task b = env.add("b");
    env.acknowledgeAll(1);
    QVERIFY(env.repo.deleteTask(a.id));
    env.acknowledgeAll(2); // a: acknowledged tombstone
    QVERIFY(env.repo.deleteTask(b.id)); // b: delete still queued
    QCOMPARE(env.count("sync_tombstones"), 2);

    const QString soon = QDateTime::currentDateTimeUtc().addDays(10).toString(Qt::ISODateWithMs);
    QVERIFY(store.cleanupTombstones(soon));
    QCOMPARE(env.count("sync_tombstones"), 2); // too young

    const QString later = QDateTime::currentDateTimeUtc().addDays(45).toString(Qt::ISODateWithMs);
    QVERIFY(store.cleanupTombstones(later));
    QCOMPARE(env.count("sync_tombstones"), 1); // b's delete is still queued: its record stays
    QCOMPARE(scalarOf(env, "SELECT task_uuid FROM sync_tombstones").toString(), b.uuid);
    QCOMPARE(env.count("sync_outbox"), 1);
    QCOMPARE(env.count("sync_base WHERE task_uuid = '" + b.uuid + "'"), 1);

    // An unreferenced base (no task, nothing queued, no record) is removed too.
    QSqlQuery q(env.db.connection());
    QVERIFY(q.exec("INSERT INTO sync_base (task_uuid, rev, snapshot, trusted) VALUES "
                   "('00000000-0000-4000-8000-00000000dead', 1, '{}', 1)"));
    QVERIFY(store.cleanupTombstones(later));
    QCOMPARE(env.count("sync_base WHERE task_uuid = '00000000-0000-4000-8000-00000000dead'"), 0);
    QVERIFY(!store.cleanupTombstones("not a time"));
}

// F12: a pull page is checked against the stored epoch like a push answer.
void TestSync::pullPagesAreCheckedAgainstTheEpoch()
{
    Env env;
    SyncStore &store = env.repo.sync();
    using P = SyncStore::PullOutcome;
    QCOMPARE(store.applyPullPage({}, {}, QVariant(), "e1"), P::NotLinked);
    QVERIFY(store.setLinked(true, "dev-a"));
    QCOMPARE(store.applyPullPage({}, {}, QVariant(), ""), P::Failed); // no epoch: malformed
    QVERIFY(store.stateValue("epoch").isEmpty());

    Task t = env.add("x");
    env.acknowledgeAll(1); // remembers e1
    QCOMPARE(store.stateValue("epoch"), QString("e1"));
    QJsonObject copy = baseSnapshot(env, t.uuid);
    copy["title"] = "from e2";
    QCOMPARE(store.applyPullPage({pullTask(t.uuid, 5, copy)}, {}, QString("9"), "e2"), P::EpochChanged);
    Task now;
    QVERIFY(env.repo.getTask(t.id, &now));
    QCOMPARE(now.title, QString("x")); // nothing applied
    QVERIFY(store.stateValue("cursor").isEmpty());
    QCOMPARE(store.stateValue("epoch"), QString("e1"));

    copy["title"] = "from e1";
    QCOMPARE(store.applyPullPage({pullTask(t.uuid, 5, copy)}, {}, QString("9"), "e1"), P::Applied);
    QVERIFY(env.repo.getTask(t.id, &now));
    QCOMPARE(now.title, QString("from e1"));
    QCOMPARE(store.stateValue("cursor"), QString("9"));

    // The first epoch a device sees is remembered together with the page that brought it.
    Env fresh;
    SyncStore &fs = fresh.repo.sync();
    QVERIFY(fs.setLinked(true, "dev-a"));
    QCOMPARE(fs.applyPullPage({}, {}, QString("1"), "e9"), P::Applied);
    QCOMPARE(fs.stateValue("epoch"), QString("e9"));
    QCOMPARE(fs.applyPullPage({}, {}, QString("2"), "e8"), P::EpochChanged);
    QCOMPARE(fs.stateValue("cursor"), QString("1"));
}

// F12: unlinking leaves holds in place (the M1 behaviour), so a later tombstone cannot remove
// content the user has not decided about.
void TestSync::heldTaskSurvivesUnlinkAndRelink()
{
    Env env;
    SyncStore &store = env.repo.sync();
    QVERIFY(store.setLinked(true, "dev-a"));
    Task t = env.add("kept content");
    env.acknowledgeAll(1);
    holdAll(env, 2);
    QVERIFY(store.setLinked(false));
    QVERIFY(store.setLinked(true, "dev-a"));
    QVERIFY(store.applyPullPage({pullTombstone(t.uuid, 4)}, {}, QVariant()));
    QCOMPARE(env.count("tasks"), 1);
    QCOMPARE(env.count("tasks WHERE held IS NOT NULL"), 1);
    QCOMPARE(scalarOf(env, "SELECT held_rev FROM tasks").toInt(), 4);
}

// F12: the per-task outbox lookup uses the task index, not a scan of the whole outbox.
void TestSync::outboxLookupByTaskUsesTheIndex()
{
    Env env;
    QSqlQuery q(env.db.connection());
    QVERIFY(q.exec("EXPLAIN QUERY PLAN SELECT seq FROM sync_outbox WHERE task_uuid = 'x' ORDER BY seq"));
    QString plan;
    while (q.next())
        plan += q.value(3).toString() + "\n";
    QVERIFY2(plan.contains("idx_sync_outbox_task"), qPrintable(plan));
    QVERIFY2(!plan.contains("USE TEMP B-TREE"), qPrintable(plan));
}

// ===========================================================================
// Conformance fixtures
// ===========================================================================

void TestSync::conformance_data()
{
    QTest::addColumn<QString>("name");
    const QJsonObject file = loadCases();
    QVERIFY2(file.value("format").toString() == "tasknest-conformance-client/1", "fixture file missing");
    for (const QJsonValue &v : file.value("cases").toArray()) {
        const QString kind = v.toObject().value("kind").toString();
        if (kImplementedKinds.contains(kind))
            QTest::newRow(qPrintable(v.toObject().value("name").toString())) << v.toObject().value("name").toString();
    }
}

void TestSync::conformance()
{
    QFETCH(QString, name);
    QJsonObject c;
    for (const QJsonValue &v : loadCases().value("cases").toArray()) {
        if (v.toObject().value("name").toString() == name)
            c = v.toObject();
    }
    const QString kind = c.value("kind").toString();
    const QJsonObject in = c.value("input").toObject();
    const QJsonObject expect = c.value("expect").toObject();

    Fixture fx;
    SyncStore &store = fx.repo.sync();
    QJsonObject local = in.value("local").toObject();
    QJsonObject actual;
    QJsonObject expectLocalHolder;
    auto opIds = [&](const QString &id) { store.setOpIdGenerator([id] { return id; }); };

    if (kind == "outbox_write" || kind == "outbox_undo_delete" || kind == "outbox_compact") {
        local = QJsonObject{{"outbox", in.value("outbox")}};
        fx.load(local);
    } else if (kind == "conflict_store" || kind == "conflict_cleanup") {
        fx.load(QJsonObject{});
    } else {
        fx.load(local);
    }

    if (kind == "outbox_write") {
        QVERIFY2(store.enqueueOperation(in.value("op").toObject()), qPrintable(store.lastError()));
        actual = fx.read();
    } else if (kind == "outbox_undo_delete") {
        bool cancelled = false;
        const QString uuid = in.value("task_id").toString();
        QVERIFY(store.recordUndoDelete(defaultTask(uuid), &cancelled));
        actual = fx.read();
        actual["cancelled"] = cancelled;
    } else if (kind == "outbox_compact") {
        QList<QByteArray> batch;
        QVERIFY(store.prepareBatch(&batch));
        QJsonArray ops;
        for (const QByteArray &p : batch)
            ops.append(QJsonDocument::fromJson(p).object());
        actual["batch"] = ops;
    } else if (kind == "apply_pull") {
        QVERIFY2(store.applyPullPage(QJsonArray{fillItem(in.value("change").toObject())}, {}, QVariant()),
                 qPrintable(store.lastError()));
        actual = QJsonObject{{"local", fx.read()}};
    } else if (kind == "apply_result") {
        const QString opId = in.value("op_id").toString();
        store.setStateValue("epoch", "epoch-1");
        QString uuid;
        for (const QJsonValue &r : local.value("outbox").toArray()) {
            if (r.toObject().value("op_id").toString() == opId)
                uuid = r.toObject().value("task_id").toString();
        }
        const QJsonObject response{{"server_epoch", "epoch-1"},
                                   {"results", QJsonArray{fillResult(in.value("result").toObject(), opId, uuid)}}};
        const auto outcome = store.applyPushResponse(response, store.stateValue("link_generation").toLongLong());
        QCOMPARE(outcomeName(outcome), QString("committed"));
        actual = QJsonObject{{"local", fx.read()}};
    } else if (kind == "apply_response" || kind == "apply_response_seq") {
        const QJsonArray steps = kind == "apply_response" ? QJsonArray{in} : in.value("steps").toArray();
        QJsonArray stepResults;
        for (const QJsonValue &sv : steps) {
            const QJsonObject step = sv.toObject();
            QJsonObject response = step.value("response").toObject();
            QJsonArray results;
            for (const QJsonValue &r : response.value("results").toArray()) {
                QJsonObject ro = r.toObject();
                QString uuid;
                QSqlQuery q(fx.database.connection());
                q.prepare("SELECT task_uuid FROM sync_outbox WHERE op_id = ?");
                q.addBindValue(ro.value("op_id").toString());
                if (q.exec() && q.next())
                    uuid = q.value(0).toString();
                else
                    uuid = "10000000-0000-4000-8000-000000000001";
                results.append(fillResult(ro, ro.value("op_id").toString(), uuid));
            }
            response["results"] = results;
            const QJsonObject f = step.value("fault").toObject();
            SyncStore::AckFault fault;
            fault.beforeApply = f.value("before_apply").toBool();
            fault.duringResult = f.value("during_result").toInt();
            fault.beforeCommit = f.value("before_commit").toBool();
            fault.afterCommit = f.value("after_commit").toBool();
            const qint64 gen = in.contains("request_link_generation") && kind == "apply_response"
                ? in.value("request_link_generation").toInt()
                : store.stateValue("link_generation").toLongLong();
            const auto outcome = store.applyPushResponse(response, gen, fault);
            QJsonObject sr{{"outcome", outcomeName(outcome)}, {"local", fx.read()}};
            QJsonArray next;
            for (const QString &id : fx.inFlightOps())
                next.append(id);
            sr["next_batch"] = next;
            stepResults.append(sr);
        }
        if (kind == "apply_response") {
            actual = stepResults.first().toObject();
        } else {
            actual["steps"] = stepResults;
            actual["local"] = fx.read();
        }
    } else if (kind == "reconcile_apply") {
        QJsonArray stepResults;
        const QJsonObject server = fillItem(in.value("server").toObject());
        for (const QJsonValue &sv : in.value("steps").toArray()) {
            const bool fail = sv.toObject().value("fault").toString() == "before_commit";
            const bool ok = store.reconcileTask(server, fail);
            QCOMPARE(ok, !fail);
            stepResults.append(QJsonObject{{"local", fx.read()}});
        }
        actual["steps"] = stepResults;
        actual["local"] = fx.read();
    } else if (kind == "held_transition") {
        const QString uuid = in.value("task_id").toString();
        const QJsonObject ev = in.value("event").toObject();
        actual["ask"] = false;
        actual["reconcile"] = false;
        if (ev.contains("edit")) {
            Task t;
            QVERIFY(fx.repo.getTask(fx.idOf(uuid), &t));
            t.title = ev.value("edit").toObject().value("title").toString(t.title);
            QVERIFY(fx.repo.updateTask(t));
        } else if (ev.contains("keep")) {
            QVERIFY2(store.holdKeep(uuid), qPrintable(store.lastError()));
        } else if (ev.contains("delete_local")) {
            QVERIFY2(store.holdDeleteLocal(uuid), qPrintable(store.lastError()));
        } else if (ev.contains("restore_result")) {
            bool ask = false, reconcile = false;
            QVERIFY(store.holdRestoreRejected(uuid, ev.value("server_state").toString() == "tombstoned",
                                              ev.value("tombstone_rev").toInt(), &ask, &reconcile));
            actual["ask"] = ask;
            actual["reconcile"] = reconcile;
        }
        actual["local"] = fx.read();
    } else if (kind == "local_mutation") {
        bool changed = false;
        QVERIFY(fx.repo.markReminderSent(fx.idOf(in.value("task_id").toString()), &changed));
        actual = QJsonObject{{"local", fx.read()}};
    } else if (kind == "local_edit") {
        opIds(in.value("op_id").toString());
        Task t;
        QVERIFY(fx.repo.getTask(fx.idOf(in.value("task_id").toString()), &t));
        const QJsonObject edit = in.value("edit").toObject();
        if (edit.contains("title")) t.title = edit.value("title").toString();
        if (edit.contains("notes")) t.notes = edit.value("notes").toString();
        if (edit.contains("priority")) t.priority = edit.value("priority").toInt();
        QVERIFY2(fx.repo.updateTask(t), qPrintable(fx.repo.lastError()));
        actual = QJsonObject{{"local", fx.read()}};
    } else if (kind == "conflict_store") {
        for (const QJsonValue &r : in.value("records").toArray())
            QVERIFY(store.storeConflict(r.toObject()));
        for (const QJsonValue &r : in.value("new").toArray())
            QVERIFY(store.storeConflict(r.toObject()));
        actual["records"] = fx.readConflicts();
    } else if (kind == "conflict_cleanup") {
        // Seeded directly: the fixture's records share a local uniqueness key on purpose.
        for (const QJsonValue &v : in.value("records").toArray()) {
            const QJsonObject r = v.toObject();
            QVERIFY(fx.sql("INSERT INTO sync_conflicts (id, source, kind, task_uuid, field_group, mine_value, "
                           "theirs_value, base_value, server_rev_seen, detected_at, state, resolved_at) "
                           "VALUES (?, 'server', 'reconcile', ?, 'title', '[\"Mine\"]', '[\"Theirs\"]', '[\"Task 1\"]', "
                           "?, ?, ?, ?)",
                           {r.value("id").toString(), r.value("task_id").toString(), r.value("server_rev_seen").toInt(),
                            r.contains("detected_at") ? r.value("detected_at").toString() : QString("2026-01-01T00:00:00.000Z"),
                            r.value("state").toString(),
                            r.contains("resolved_at") ? QVariant(r.value("resolved_at").toString()) : nullStr()}));
        }
        QVERIFY(store.cleanupConflicts(in.value("now").toString()));
        actual["records"] = fx.readConflicts();
    } else if (kind == "conflict_resolve") {
        opIds(in.value("op_id").toString());
        SyncStore::ConflictResolution res;
        QVERIFY2(store.resolveConflict(in.value("record_id").toString(), in.value("action").toString(),
                                       in.value("now").toString(), &res),
                 qPrintable(store.lastError()));
        actual = QJsonObject{{"result", res.result}, {"local", fx.read()}};
        if (!res.current.isUndefined())
            actual["current"] = res.current;
    } else {
        QFAIL(qPrintable("unhandled fixture kind " + kind));
    }

    const QString error = subset(expect, actual, "");
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void TestSync::conformanceCoversTheWholeFixtureFile()
{
    // Cases of kinds that belong to the client engine milestone (full first-link
    // reconciliation and old-row folding) are the only ones not run here; every
    // other kind in the file must be handled above.
    const QStringList deferred = {"rebaseline", "fold_old"};
    int run = 0, left = 0;
    for (const QJsonValue &v : loadCases().value("cases").toArray()) {
        const QString kind = v.toObject().value("kind").toString();
        if (kImplementedKinds.contains(kind))
            ++run;
        else {
            ++left;
            QVERIFY2(deferred.contains(kind), qPrintable("unknown fixture kind " + kind));
        }
    }
    qInfo("conformance: %d cases run, %d deferred (%s)", run, left, qPrintable(deferred.join(", ")));
    QVERIFY(run > 70);
}

QTEST_GUILESS_MAIN(TestSync)

#include "tst_sync.moc"
