#include "syncstore.h"

#include "database.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {

const QString kCompleted = QStringLiteral("completion");

QString nowText()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QVariant nullText()
{
    return QVariant(QMetaType::fromType<QString>());
}

QVariant nullInt()
{
    return QVariant(QMetaType::fromType<qlonglong>());
}

// Runs one statement with bound values. `affected` receives the row count.
bool runSql(const QSqlDatabase &db, const QString &sql, const QVariantList &binds, QString *error,
            int *affected = nullptr)
{
    QSqlQuery query(db);
    if (!query.prepare(sql)) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    for (const QVariant &value : binds)
        query.addBindValue(value);
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    if (affected)
        *affected = query.numRowsAffected();
    return true;
}

// Prepared and executed SELECT; the caller iterates `query`.
bool selectSql(const QSqlDatabase &db, QSqlQuery *query, const QString &sql, const QVariantList &binds,
               QString *error)
{
    *query = QSqlQuery(db);
    if (!query->prepare(sql)) {
        if (error)
            *error = query->lastError().text();
        return false;
    }
    for (const QVariant &value : binds)
        query->addBindValue(value);
    if (!query->exec()) {
        if (error)
            *error = query->lastError().text();
        return false;
    }
    return true;
}

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject parseObject(const QVariant &text)
{
    if (text.isNull())
        return {};
    return QJsonDocument::fromJson(text.toString().toUtf8()).object();
}

QStringList parseStrings(const QVariant &text)
{
    QStringList result;
    if (text.isNull())
        return result;
    const QJsonArray array = QJsonDocument::fromJson(text.toString().toUtf8()).array();
    for (const QJsonValue &value : array)
        result << value.toString();
    return result;
}

QString stringsToJson(QStringList list)
{
    list.removeDuplicates();
    list.sort();
    QJsonArray array;
    for (const QString &item : list)
        array.append(item);
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

// A group value is stored wrapped in an array so JSON scalars (and null) round-trip.
QVariant encodeValue(const QJsonValue &value)
{
    if (value.isUndefined())
        return nullText();
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

QJsonValue decodeValue(const QVariant &text)
{
    if (text.isNull())
        return QJsonValue::Undefined;
    const QJsonArray array = QJsonDocument::fromJson(text.toString().toUtf8()).array();
    return array.isEmpty() ? QJsonValue::Undefined : array.at(0);
}

QVariant sqlFromJson(const QJsonValue &value)
{
    if (value.isNull() || value.isUndefined())
        return nullText();
    if (value.isBool())
        return value.toBool() ? 1 : 0;
    if (value.isDouble())
        return static_cast<qlonglong>(value.toDouble());
    return value.toString();
}

QJsonValue jsonFromSql(const QVariant &value)
{
    return value.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue::fromVariant(value);
}

// Every synced field present, `id` set; missing fields get their schema defaults.
QJsonObject normalizeTask(const QJsonObject &input, const QString &uuid)
{
    QJsonObject task = input;
    task.insert(QStringLiteral("id"), uuid);
    const struct { const char *key; QJsonValue value; } defaults[] = {
        {"title", QString()},
        {"notes", QString()},
        {"list_name", QStringLiteral("Inbox")},
        {"priority", 0},
        {"due_at", QJsonValue(QJsonValue::Null)},
        {"completed", false},
        {"completed_at", QJsonValue(QJsonValue::Null)},
        {"reminder_at", QJsonValue(QJsonValue::Null)},
        {"created_at", QStringLiteral("1970-01-01T00:00:00.000Z")},
    };
    for (const auto &entry : defaults) {
        const QString key = QLatin1String(entry.key);
        if (!task.contains(key))
            task.insert(key, entry.value);
    }
    return task;
}

QString jsonToText(const QJsonObject &object)
{
    return QString::fromUtf8(SyncStore::compactJson(object));
}

QVariant intOrNull(const QJsonValue &value)
{
    if (value.isNull() || value.isUndefined())
        return nullInt();
    return static_cast<qlonglong>(value.toInt());
}

// A revision from the wire: a whole number of at least `minimum`.
bool wholeRev(const QJsonValue &value, int minimum = 1)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return number == std::floor(number) && number >= minimum && number <= 2147483647.0;
}

} // namespace

// ---- DbTransaction -----------------------------------------------------

DbTransaction::DbTransaction(QSqlDatabase db)
    : m_db(std::move(db))
{
}

DbTransaction::~DbTransaction()
{
    rollback();
}

bool DbTransaction::begin()
{
    // IMMEDIATE takes the write lock now, so a later statement cannot fail
    // half-way with SQLITE_BUSY after others have succeeded.
    if (!runSql(m_db, QStringLiteral("BEGIN IMMEDIATE"), {}, &m_error))
        return false;
    m_active = true;
    return true;
}

bool DbTransaction::commit()
{
    if (!m_active)
        return false;
    if (!runSql(m_db, QStringLiteral("COMMIT"), {}, &m_error)) {
        rollback();
        return false;
    }
    m_active = false;
    return true;
}

void DbTransaction::rollback()
{
    if (!m_active)
        return;
    QString ignored;
    runSql(m_db, QStringLiteral("ROLLBACK"), {}, &ignored);
    m_active = false;
}

// ---- SyncStore internals -----------------------------------------------

struct SyncStore::Row
{
    qint64 seq = 0;
    QString opId;
    QString type;
    QString taskUuid;
    QString settingKey;
    QVariant baseRev = nullInt();
    QString afterOp;
    bool inFlight = false;
    QStringList changed;
    QJsonObject task;
    QString reason;
    QString value;
    QVariant authoredRev = nullInt();
    bool collided = false;
    QString restoreOf;
    int attempts = 0;
    QString queuedAt;
    QString sentPayload;
};

struct SyncStore::TaskInfo
{
    qint64 localId = 0;
    QVariant serverRev = nullInt();
    QString held;
    QVariant heldRev = nullInt();
    QString heldDeletedAt;
    QString notSynced;
};

namespace {

const QString kRowColumns = QStringLiteral(
    "seq, op_id, type, task_uuid, setting_key, base_rev, after_op_id, state, changed, task, reason, "
    "value, authored_rev, collided, restore_of, attempts, queued_at, sent_payload");

} // namespace

SyncStore::SyncStore(Database &database)
    : m_database(database)
{
}

QSqlDatabase SyncStore::db() const
{
    return m_database.connection();
}

bool SyncStore::fail(const QString &message)
{
    m_lastError = message;
    return false;
}

QString SyncStore::newOpId() const
{
    return m_opId ? m_opId() : newUuid();
}

const QStringList &SyncStore::groups()
{
    static const QStringList list = {QStringLiteral("title"), QStringLiteral("notes"),
                                     QStringLiteral("list_name"), QStringLiteral("priority"),
                                     QStringLiteral("due_at"), QStringLiteral("reminder_at"), kCompleted};
    return list;
}

QJsonValue SyncStore::groupValue(const QJsonObject &task, const QString &group)
{
    auto field = [&](const QString &key) {
        const QJsonValue value = task.value(key);
        return value.isUndefined() ? QJsonValue(QJsonValue::Null) : value;
    };
    if (group == kCompleted) {
        return QJsonObject{{QStringLiteral("completed"), field(QStringLiteral("completed"))},
                           {QStringLiteral("completed_at"), field(QStringLiteral("completed_at"))}};
    }
    return field(group);
}

QStringList SyncStore::differingGroups(const QJsonObject &a, const QJsonObject &b)
{
    QStringList result;
    for (const QString &group : groups()) {
        if (groupValue(a, group) != groupValue(b, group))
            result << group;
    }
    result.sort();
    return result;
}

QByteArray SyncStore::compactJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

namespace {

void setGroup(QJsonObject &task, const QString &group, const QJsonValue &value)
{
    if (group == kCompleted) {
        const QJsonObject pair = value.toObject();
        task.insert(QStringLiteral("completed"), pair.value(QStringLiteral("completed")));
        task.insert(QStringLiteral("completed_at"), pair.value(QStringLiteral("completed_at")));
    } else {
        task.insert(group, value);
    }
}

} // namespace

// ---- state -------------------------------------------------------------

QString SyncStore::stateValue(const QString &key, const QString &fallback) const
{
    QSqlQuery query;
    QString error;
    if (!selectSql(db(), &query, QStringLiteral("SELECT value FROM sync_state WHERE key = ?"), {key}, &error)
        || !query.next())
        return fallback;
    return query.value(0).toString();
}

bool SyncStore::setStateValue(const QString &key, const QString &value)
{
    QString error;
    if (!runSql(db(), QStringLiteral("INSERT INTO sync_state (key, value) VALUES (?, ?) "
                                     "ON CONFLICT(key) DO UPDATE SET value = excluded.value"),
                {key, value}, &error))
        return fail(error);
    return true;
}

bool SyncStore::isLinked() const
{
    return stateValue(QStringLiteral("linked")) == QLatin1String("1");
}

bool SyncStore::setLinked(bool linked, const QString &deviceId)
{
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());
    const qint64 generation = stateValue(QStringLiteral("link_generation"), QStringLiteral("0")).toLongLong() + 1;
    if (!setStateValue(QStringLiteral("linked"), linked ? QStringLiteral("1") : QStringLiteral("0"))
        || !setStateValue(QStringLiteral("link_generation"), QString::number(generation)))
        return false;
    if (!deviceId.isEmpty() && !setStateValue(QStringLiteral("device_id"), deviceId))
        return false;
    if (!tx.commit())
        return fail(tx.error());
    return true;
}

// ---- tasks -------------------------------------------------------------

bool SyncStore::taskInfo(const QString &uuid, TaskInfo *info) const
{
    QSqlQuery query;
    QString error;
    if (!selectSql(db(), &query,
                   QStringLiteral("SELECT id, server_rev, held, held_rev, held_deleted_at, not_synced "
                                  "FROM tasks WHERE uuid = ?"),
                   {uuid}, &error)
        || !query.next())
        return false;
    if (info) {
        info->localId = query.value(0).toLongLong();
        info->serverRev = query.value(1).isNull() ? nullInt() : QVariant(query.value(1).toLongLong());
        info->held = query.value(2).toString();
        info->heldRev = query.value(3).isNull() ? nullInt() : QVariant(query.value(3).toLongLong());
        info->heldDeletedAt = query.value(4).toString();
        info->notSynced = query.value(5).toString();
    }
    return true;
}

bool SyncStore::loadTask(const QString &uuid, QJsonObject *task, TaskInfo *info) const
{
    QSqlQuery query;
    QString error;
    if (!selectSql(db(), &query,
                   QStringLiteral("SELECT title, notes, list_name, priority, due_at, completed, completed_at, "
                                  "reminder_at, created_at FROM tasks WHERE uuid = ?"),
                   {uuid}, &error)
        || !query.next())
        return false;
    if (task) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), uuid);
        object.insert(QStringLiteral("title"), query.value(0).toString());
        object.insert(QStringLiteral("notes"), query.value(1).toString());
        object.insert(QStringLiteral("list_name"), query.value(2).toString());
        object.insert(QStringLiteral("priority"), query.value(3).toInt());
        object.insert(QStringLiteral("due_at"), jsonFromSql(query.value(4)));
        object.insert(QStringLiteral("completed"), query.value(5).toInt() != 0);
        object.insert(QStringLiteral("completed_at"), jsonFromSql(query.value(6)));
        object.insert(QStringLiteral("reminder_at"), jsonFromSql(query.value(7)));
        object.insert(QStringLiteral("created_at"), query.value(8).toString());
        *task = object;
    }
    return info ? taskInfo(uuid, info) : true;
}

bool SyncStore::loadBase(const QString &uuid, QJsonObject *snapshot, int *rev) const
{
    QSqlQuery query;
    QString error;
    if (!selectSql(db(), &query, QStringLiteral("SELECT rev, snapshot FROM sync_base WHERE task_uuid = ?"),
                   {uuid}, &error)
        || !query.next())
        return false;
    if (rev)
        *rev = query.value(0).toInt();
    if (snapshot)
        *snapshot = parseObject(query.value(1));
    return true;
}

bool SyncStore::baseTrusted(const QString &uuid) const
{
    QSqlQuery query;
    QString error;
    if (!selectSql(db(), &query, QStringLiteral("SELECT trusted FROM sync_base WHERE task_uuid = ?"), {uuid}, &error)
        || !query.next())
        return true; // no base: there is no revision to distrust
    return query.value(0).toInt() != 0;
}

bool SyncStore::storeBase(const QString &uuid, int rev, const QJsonObject &snapshot)
{
    QString error;
    // Forward only (B2): a copy older than the stored base is ignored.
    int storedRev = 0;
    if (loadBase(uuid, nullptr, &storedRev) && storedRev > rev)
        return true;
    // rev and snapshot move together, and only from a server-supplied copy (B1, B2).
    if (!runSql(db(),
                QStringLiteral("INSERT INTO sync_base (task_uuid, rev, snapshot, epoch, trusted) "
                               "VALUES (?, ?, ?, ?, 1) ON CONFLICT(task_uuid) DO UPDATE SET "
                               "rev = excluded.rev, snapshot = excluded.snapshot, "
                               "epoch = excluded.epoch, trusted = 1"),
                {uuid, rev, jsonToText(normalizeTask(snapshot, uuid)), stateValue(QStringLiteral("epoch"))},
                &error))
        return fail(error);
    return setServerRev(uuid, rev);
}

bool SyncStore::setServerRev(const QString &uuid, const QVariant &rev)
{
    QString error;
    if (!runSql(db(), QStringLiteral("UPDATE tasks SET server_rev = ? WHERE uuid = ?"), {rev, uuid}, &error))
        return fail(error);
    return true;
}

bool SyncStore::writeTaskFields(const QString &uuid, const QJsonObject &task)
{
    QString error;
    // `reminder_at IS ?` compares with the value before this update, so a
    // changed reminder time re-arms the (device-local) reminder, as today.
    const QVariant reminder = sqlFromJson(task.value(QStringLiteral("reminder_at")));
    if (!runSql(db(),
                QStringLiteral("UPDATE tasks SET title = ?, notes = ?, list_name = ?, priority = ?, due_at = ?, "
                               "completed = ?, completed_at = ?, "
                               "reminder_sent = CASE WHEN reminder_at IS ? THEN reminder_sent ELSE 0 END, "
                               "reminder_at = ?, updated_at = ? WHERE uuid = ?"),
                {task.value(QStringLiteral("title")).toString(), task.value(QStringLiteral("notes")).toString(),
                 task.value(QStringLiteral("list_name")).toString(), task.value(QStringLiteral("priority")).toInt(),
                 sqlFromJson(task.value(QStringLiteral("due_at"))),
                 task.value(QStringLiteral("completed")).toBool() ? 1 : 0,
                 sqlFromJson(task.value(QStringLiteral("completed_at"))), reminder, reminder, nowText(), uuid},
                &error))
        return fail(error);
    return true;
}

bool SyncStore::insertTask(const QJsonObject &task, int rev)
{
    QString error;
    const QString uuid = task.value(QStringLiteral("id")).toString();
    const QString now = nowText();
    if (!runSql(db(),
                QStringLiteral("INSERT INTO tasks (uuid, title, notes, list_name, priority, due_at, completed, "
                               "completed_at, reminder_at, reminder_sent, created_at, updated_at, server_rev) "
                               "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?)"),
                {uuid, task.value(QStringLiteral("title")).toString(), task.value(QStringLiteral("notes")).toString(),
                 task.value(QStringLiteral("list_name")).toString(), task.value(QStringLiteral("priority")).toInt(),
                 sqlFromJson(task.value(QStringLiteral("due_at"))),
                 task.value(QStringLiteral("completed")).toBool() ? 1 : 0,
                 sqlFromJson(task.value(QStringLiteral("completed_at"))),
                 sqlFromJson(task.value(QStringLiteral("reminder_at"))),
                 task.value(QStringLiteral("created_at")).toString(), now, rev},
                &error))
        return fail(error);
    return true;
}

bool SyncStore::deleteTaskRows(const QString &uuid)
{
    QString error;
    if (!runSql(db(), QStringLiteral("DELETE FROM tasks WHERE uuid = ?"), {uuid}, &error)
        || !runSql(db(), QStringLiteral("DELETE FROM sync_base WHERE task_uuid = ?"), {uuid}, &error))
        return fail(error);
    return true;
}

bool SyncStore::setHeld(const QString &uuid, int tombstoneRev, const QString &deletedAt)
{
    QString error;
    // The recorded tombstone revision only moves forward; a stale one keeps the newer record.
    TaskInfo info;
    int rev = tombstoneRev;
    QString at = deletedAt;
    if (taskInfo(uuid, &info) && !info.held.isEmpty() && !info.heldRev.isNull()) {
        if (info.heldRev.toInt() > tombstoneRev) {
            rev = info.heldRev.toInt();
            at = info.heldDeletedAt;
        } else if (at.isEmpty()) {
            at = info.heldDeletedAt;
        }
    }
    if (!runSql(db(), QStringLiteral("UPDATE tasks SET held = 'deleted_in_cloud', held_rev = ?, "
                                     "held_deleted_at = ? WHERE uuid = ?"),
                {rev, at.isEmpty() ? nullText() : QVariant(at), uuid}, &error))
        return fail(error);
    return true;
}

// Adopts the server's copy outright (rejections that carry the winning task).
bool SyncStore::replaceWithServerCopy(const QString &uuid, const QJsonObject &task, int rev)
{
    const QJsonObject copy = normalizeTask(task, uuid);
    if (taskInfo(uuid, nullptr)) {
        if (!writeTaskFields(uuid, copy))
            return false;
    } else if (!insertTask(copy, rev)) {
        return false;
    }
    QString error;
    if (!runSql(db(), QStringLiteral("UPDATE tasks SET held = NULL, held_rev = NULL, held_deleted_at = NULL, "
                                     "not_synced = NULL WHERE uuid = ?"),
                {uuid}, &error)
        || !runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error))
        return fail(error);
    return storeBase(uuid, rev, copy);
}

// ---- outbox rows -------------------------------------------------------

QList<SyncStore::Row> SyncStore::allRows() const
{
    return queryRows(QString(), {});
}

QList<SyncStore::Row> SyncStore::queryRows(const QString &where, const QVariantList &binds) const
{
    QList<Row> rows;
    QSqlQuery query;
    QString error;
    if (!selectSql(db(), &query,
                   QStringLiteral("SELECT %1 FROM sync_outbox %2 ORDER BY seq").arg(kRowColumns, where), binds,
                   &error))
        return rows;
    while (query.next()) {
        Row row;
        row.seq = query.value(0).toLongLong();
        row.opId = query.value(1).toString();
        row.type = query.value(2).toString();
        row.taskUuid = query.value(3).toString();
        row.settingKey = query.value(4).toString();
        row.baseRev = query.value(5).isNull() ? nullInt() : QVariant(query.value(5).toLongLong());
        row.afterOp = query.value(6).toString();
        row.inFlight = query.value(7).toString() == QLatin1String("in_flight");
        row.changed = parseStrings(query.value(8));
        row.task = parseObject(query.value(9));
        row.reason = query.value(10).toString();
        row.value = query.value(11).toString();
        row.authoredRev = query.value(12).isNull() ? nullInt() : QVariant(query.value(12).toLongLong());
        row.collided = query.value(13).toInt() != 0;
        row.restoreOf = query.value(14).toString();
        row.attempts = query.value(15).toInt();
        row.queuedAt = query.value(16).toString();
        row.sentPayload = query.value(17).toString();
        rows << row;
    }
    return rows;
}

QList<SyncStore::Row> SyncStore::rowsForTask(const QString &uuid) const
{
    // Uses idx_sync_outbox_task: the cost follows the task's own rows, not the whole outbox.
    return queryRows(QStringLiteral("WHERE task_uuid = ?"), {uuid});
}

bool SyncStore::insertRow(Row &row)
{
    QString error;
    int affected = 0;
    if (!runSql(db(),
                QStringLiteral("INSERT INTO sync_outbox (op_id, type, task_uuid, setting_key, base_rev, after_op_id, "
                               "state, changed, task, reason, value, authored_rev, collided, restore_of, attempts, "
                               "queued_at, sent_payload) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
                {row.opId, row.type, row.taskUuid.isEmpty() ? nullText() : QVariant(row.taskUuid),
                 row.settingKey.isEmpty() ? nullText() : QVariant(row.settingKey), row.baseRev,
                 row.afterOp.isEmpty() ? nullText() : QVariant(row.afterOp),
                 row.inFlight ? QStringLiteral("in_flight") : QStringLiteral("pending"),
                 row.type == QLatin1String("update") ? QVariant(stringsToJson(row.changed)) : nullText(),
                 row.task.isEmpty() ? nullText() : QVariant(jsonToText(row.task)),
                 row.reason.isEmpty() ? nullText() : QVariant(row.reason),
                 row.value.isEmpty() ? nullText() : QVariant(row.value), row.authoredRev, row.collided ? 1 : 0,
                 row.restoreOf.isEmpty() ? nullText() : QVariant(row.restoreOf), row.attempts,
                 row.queuedAt.isEmpty() ? nowText() : row.queuedAt,
                 row.sentPayload.isEmpty() ? nullText() : QVariant(row.sentPayload)},
                &error, &affected))
        return fail(error);
    return true;
}

bool SyncStore::updateRow(const Row &row)
{
    QString error;
    if (!runSql(db(),
                QStringLiteral("UPDATE sync_outbox SET base_rev = ?, after_op_id = ?, state = ?, changed = ?, "
                               "task = ?, authored_rev = ?, collided = ?, attempts = ?, sent_payload = ? "
                               "WHERE op_id = ?"),
                {row.baseRev, row.afterOp.isEmpty() ? nullText() : QVariant(row.afterOp),
                 row.inFlight ? QStringLiteral("in_flight") : QStringLiteral("pending"),
                 row.type == QLatin1String("update") ? QVariant(stringsToJson(row.changed)) : nullText(),
                 row.task.isEmpty() ? nullText() : QVariant(jsonToText(row.task)), row.authoredRev,
                 row.collided ? 1 : 0, row.attempts,
                 row.sentPayload.isEmpty() ? nullText() : QVariant(row.sentPayload), row.opId},
                &error))
        return fail(error);
    return true;
}

bool SyncStore::deleteRow(const QString &opId)
{
    QString error;
    if (!runSql(db(), QStringLiteral("DELETE FROM sync_outbox WHERE op_id = ?"), {opId}, &error))
        return fail(error);
    return true;
}

// 11.2: one row per operation; folding only where it is lossless and the
// earlier row is unsent. A row in flight is never modified.
bool SyncStore::writeOperation(Row row)
{
    const QList<Row> rows = rowsForTask(row.taskUuid);

    const Row *lastInFlight = nullptr;
    for (const Row &existing : rows) {
        if (existing.inFlight)
            lastInFlight = &existing;
    }
    if (lastInFlight) {
        // Waits behind the in-flight row; its base is resolved when that row is answered.
        row.afterOp = lastInFlight->opId;
        row.baseRev = nullInt();
        return insertRow(row);
    }

    if (row.type == QLatin1String("update") && row.restoreOf.isEmpty() && !rows.isEmpty()) {
        Row last = rows.last();
        if (!last.inFlight && last.restoreOf.isEmpty()) {
            if (last.type == QLatin1String("create") || last.type == QLatin1String("restore")) {
                last.task = row.task;
                return updateRow(last);
            }
            if (last.type == QLatin1String("update")) {
                last.changed << row.changed;
                last.changed.removeDuplicates();
                last.changed.sort();
                last.task = row.task;
                return updateRow(last);
            }
        }
    }
    return insertRow(row);
}

QJsonObject SyncStore::wirePayload(const Row &row) const
{
    QJsonObject op;
    op.insert(QStringLiteral("op_id"), row.opId);
    op.insert(QStringLiteral("type"), row.type);
    if (row.type == QLatin1String("set_setting")) {
        op.insert(QStringLiteral("key"), row.settingKey);
        op.insert(QStringLiteral("value"), row.value);
    } else {
        op.insert(QStringLiteral("task_id"), row.taskUuid);
    }
    op.insert(QStringLiteral("base_rev"),
              row.baseRev.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(row.baseRev.toInt()));
    if (row.type == QLatin1String("update")) {
        op.insert(QStringLiteral("origin"), QStringLiteral("user"));
        QJsonArray changed;
        for (const QString &group : row.changed)
            changed.append(group);
        op.insert(QStringLiteral("changed"), changed);
    }
    if (row.type == QLatin1String("delete"))
        op.insert(QStringLiteral("reason"), row.reason);
    if (row.type == QLatin1String("create") || row.type == QLatin1String("update")
        || row.type == QLatin1String("restore"))
        op.insert(QStringLiteral("task"), row.task);
    return op;
}

// ---- recording local changes -------------------------------------------

bool SyncStore::recordCreate(const QJsonObject &task)
{
    if (!isLinked())
        return true;
    Row row;
    row.opId = newOpId();
    row.type = QStringLiteral("create");
    row.taskUuid = task.value(QStringLiteral("id")).toString();
    row.task = task;
    return writeOperation(row);
}

bool SyncStore::recordEdit(const QJsonObject &before, const QJsonObject &after, const QString &restoreOf,
                           QString *opIdOut)
{
    if (opIdOut)
        opIdOut->clear();
    if (!isLinked())
        return true;
    const QString uuid = after.value(QStringLiteral("id")).toString();
    TaskInfo info;
    if (!taskInfo(uuid, &info))
        return fail(QStringLiteral("Unknown task %1.").arg(uuid));
    const QList<Row> rows = rowsForTask(uuid);
    // A held task is edited locally only; the user decides first (11.8). Once the
    // user chose Keep, a restore is waiting and later edits belong to it: they are
    // folded into the unsent restore or queued behind the one in flight, so the
    // acknowledgement cannot overwrite them.
    if (!info.held.isEmpty()) {
        bool restoreWaiting = false;
        for (const Row &row : rows)
            restoreWaiting = restoreWaiting || row.type == QLatin1String("restore");
        if (!restoreWaiting)
            return true;
    }

    QStringList changed = differingGroups(before, after);
    QJsonObject base;
    const bool haveBase = loadBase(uuid, &base);
    // The earlier operation was refused: describe the change against the
    // unchanged base so nothing the user typed is left out (11.8).
    const bool deliberate = !changed.isEmpty();
    // A distrusted revision is never used as a base_rev: the edit stays local, the task is
    // marked for reconciliation (9.2 recomputes it as the groups where L differs from B).
    if (haveBase && !info.serverRev.isNull() && !baseTrusted(uuid)) {
        if (!deliberate || !info.notSynced.isEmpty())
            return true;
        QString error;
        if (!runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = 'needs_reconcile' WHERE uuid = ?"), {uuid},
                    &error))
            return fail(error);
        return true;
    }
    if (!info.notSynced.isEmpty() && haveBase)
        changed = differingGroups(base, after);
    if (changed.isEmpty()) {
        // Not a deliberate edit: the "not synced" mark stays. An edit that brings the task back
        // to its base leaves nothing unsynced, so the mark ends there.
        if (deliberate && !info.notSynced.isEmpty()) {
            QString error;
            if (!runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = NULL WHERE uuid = ?"), {uuid}, &error))
                return fail(error);
        }
        return true;
    }
    if (!info.notSynced.isEmpty()) {
        QString error;
        if (!runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = NULL WHERE uuid = ?"), {uuid}, &error))
            return fail(error);
    }

    bool hasCreate = false;
    for (const Row &row : rows)
        hasCreate = hasCreate || row.type == QLatin1String("create");

    Row row;
    row.opId = newOpId();
    row.taskUuid = uuid;
    row.task = after;
    row.restoreOf = restoreOf;
    if (info.serverRev.isNull() && !hasCreate) {
        // The server has never seen this task: it needs a create, not an update.
        row.type = QStringLiteral("create");
        row.restoreOf.clear();
    } else {
        row.type = QStringLiteral("update");
        row.changed = changed;
        row.baseRev = info.serverRev;
        row.authoredRev = info.serverRev;
    }
    if (!writeOperation(row))
        return false;
    if (opIdOut)
        *opIdOut = row.opId;
    return true;
}

bool SyncStore::recordDelete(const QJsonObject &task, qint64 localId, const QString &reason)
{
    if (!isLinked())
        return true;
    const QString uuid = task.value(QStringLiteral("id")).toString();
    TaskInfo info;
    if (!taskInfo(uuid, &info))
        return true;
    // Deleting a held task is "delete locally": the server already has the tombstone,
    // so nothing is queued. The caller removes the tasks row; the base goes with it,
    // as in holdDeleteLocal/deleteTaskRows, so no orphaned sync_base row remains.
    if (!info.held.isEmpty())
        return discardHeldRows(uuid, localId, reason);

    const QList<Row> rows = rowsForTask(uuid);
    bool hasCreate = false;
    for (const Row &row : rows)
        hasCreate = hasCreate || row.type == QLatin1String("create") || row.type == QLatin1String("restore");
    if (info.serverRev.isNull() && !hasCreate)
        return true; // the server never saw it

    QString error;
    if (!runSql(db(),
                QStringLiteral("INSERT INTO sync_tombstones (task_uuid, local_id, server_rev, deleted_at, reason, "
                               "tombstone_rev) VALUES (?, ?, ?, ?, ?, NULL) ON CONFLICT(task_uuid) DO UPDATE SET "
                               "local_id = excluded.local_id, server_rev = excluded.server_rev, "
                               "deleted_at = excluded.deleted_at, reason = excluded.reason, tombstone_rev = NULL, "
                               "held_rev = NULL, held_deleted_at = NULL"),
                {uuid, localId, info.serverRev, nowText(), reason}, &error))
        return fail(error);

    Row row;
    row.opId = newOpId();
    row.type = QStringLiteral("delete");
    row.taskUuid = uuid;
    row.baseRev = info.serverRev;
    row.reason = reason;
    return writeOperation(row);
}

bool SyncStore::discardHeldRows(const QString &uuid, qint64 localId, const QString &reason)
{
    QString error;
    const QList<Row> rows = rowsForTask(uuid);
    bool restoreInFlight = false;
    for (const Row &row : rows)
        restoreInFlight = restoreInFlight || (row.type == QLatin1String("restore") && row.inFlight);
    // Unsent rows (a pending restore, edits folded into it or waiting behind it) are
    // moot once the user deleted the task: cancel them.
    for (const Row &row : rows) {
        if (!row.inFlight && (!deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf)))
            return false;
    }
    if (!restoreInFlight) {
        if (!runSql(db(), QStringLiteral("DELETE FROM sync_base WHERE task_uuid = ?"), {uuid}, &error))
            return fail(error);
        return true; // the server already has the tombstone: nothing to send
    }

    // A restore on its way may still be applied; the delete follows its answer so the
    // task cannot reappear on the server (resolved under 11.7 like any dependent row).
    // The base stays (like for any delete in flight) and the hold is remembered in the
    // record, so an Undo before the answer can put the task back exactly as it was.
    TaskInfo info;
    taskInfo(uuid, &info);
    if (!runSql(db(),
                QStringLiteral("INSERT INTO sync_tombstones (task_uuid, local_id, server_rev, deleted_at, reason, "
                               "tombstone_rev, held_rev, held_deleted_at) VALUES (?, ?, ?, ?, ?, NULL, ?, ?) "
                               "ON CONFLICT(task_uuid) DO UPDATE SET "
                               "local_id = excluded.local_id, server_rev = excluded.server_rev, "
                               "deleted_at = excluded.deleted_at, reason = excluded.reason, tombstone_rev = NULL, "
                               "held_rev = excluded.held_rev, held_deleted_at = excluded.held_deleted_at"),
                {uuid, localId, info.serverRev, nowText(), reason, info.heldRev.isNull() ? QVariant(0) : info.heldRev,
                 info.heldDeletedAt.isEmpty() ? nullText() : QVariant(info.heldDeletedAt)}, &error))
        return fail(error);
    Row row;
    row.opId = newOpId();
    row.type = QStringLiteral("delete");
    row.taskUuid = uuid;
    row.reason = reason;
    return writeOperation(row);
}

bool SyncStore::recordUndoDelete(const QJsonObject &task, bool *cancelled)
{
    if (cancelled)
        *cancelled = false;
    if (!isLinked())
        return true;
    const QString uuid = task.value(QStringLiteral("id")).toString();
    const QList<Row> rows = rowsForTask(uuid);
    QString error;

    const Row *pendingDelete = nullptr;
    for (const Row &row : rows) {
        if (row.type == QLatin1String("delete"))
            pendingDelete = &row;
    }
    if (pendingDelete && !pendingDelete->inFlight) {
        // The task was held, the user chose Keep, and then deleted it while that restore
        // is still in flight: put the task back exactly as it was (held, on the same
        // restore), so the answer is applied to it as if nothing had happened.
        const Row *restoreInFlight = nullptr;
        for (const Row &row : rows) {
            if (row.type == QLatin1String("restore") && row.inFlight && row.opId == pendingDelete->afterOp)
                restoreInFlight = &row;
        }
        QSqlQuery record;
        const bool wasHeld = restoreInFlight
            && selectSql(db(), &record,
                         QStringLiteral("SELECT held_rev, held_deleted_at FROM sync_tombstones WHERE task_uuid = ?"),
                         {uuid}, &error)
            && record.next() && !record.value(0).isNull();
        if (wasHeld) {
            // 0 stands for "held, tombstone revision unknown" (revisions start at 1).
            const QVariant heldRev = record.value(0).toLongLong() == 0 ? nullInt() : QVariant(record.value(0).toLongLong());
            const QVariant heldAt = record.value(1).isNull() ? nullText() : QVariant(record.value(1).toString());
            QJsonObject base;
            int baseRev = 0;
            TaskInfo info;
            if (!taskInfo(uuid, &info))
                return fail(QStringLiteral("Unknown task %1.").arg(uuid));
            if (!deleteRow(pendingDelete->opId)
                || !runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error)
                || !runSql(db(), QStringLiteral("UPDATE tasks SET held = 'deleted_in_cloud', held_rev = ?, "
                                                "held_deleted_at = ? WHERE uuid = ?"),
                           {heldRev, heldAt, uuid}, &error))
                return error.isEmpty() ? false : fail(error);
            // The base survived the delete; the revision comes from it, never from the snapshot.
            if (loadBase(uuid, &base, &baseRev) && !setServerRev(uuid, baseRev))
                return false;
            // Edits made after Keep were cancelled with the delete; they are local content
            // again, so they wait behind the restore like any edit made after Keep (B3).
            if (!recordEdit(restoreInFlight->task, task))
                return false;
            if (cancelled)
                *cancelled = true;
            return true;
        }
        // Never sent: cancel it (earlier rows of the task are untouched).
        if (!deleteRow(pendingDelete->opId)
            || !runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error))
            return error.isEmpty() ? false : fail(error);
        // The re-inserted row does not carry the "not synced" mark. If no waiting operation
        // carries the task's content and it differs from its base, it is flagged again so the
        // content is not left unrepresented (B3).
        QJsonObject base;
        if (rowsForTask(uuid).isEmpty() && loadBase(uuid, &base) && !differingGroups(base, task).isEmpty()
            && !runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = 'needs_reconcile' WHERE uuid = ? "
                                            "AND not_synced IS NULL"),
                       {uuid}, &error))
            return fail(error);
        if (cancelled)
            *cancelled = true;
        return true;
    }

    Row row;
    row.opId = newOpId();
    row.taskUuid = uuid;
    row.task = task;
    if (pendingDelete) { // in flight: the restore waits for its answer
        row.type = QStringLiteral("restore");
        return writeOperation(row);
    }

    QSqlQuery query;
    if (selectSql(db(), &query, QStringLiteral("SELECT tombstone_rev FROM sync_tombstones WHERE task_uuid = ?"),
                  {uuid}, &error)
        && query.next() && !query.value(0).isNull()) {
        row.type = QStringLiteral("restore");
        row.baseRev = query.value(0).toLongLong();
        return writeOperation(row);
    }

    // No delete row and no acknowledged tombstone: the delete was compacted away
    // (the server never saw the task). Make sure the task still gets created.
    TaskInfo info;
    bool hasCreate = false;
    for (const Row &existing : rows)
        hasCreate = hasCreate || existing.type == QLatin1String("create");
    if (taskInfo(uuid, &info) && info.serverRev.isNull() && !hasCreate) {
        row.type = QStringLiteral("create");
        return writeOperation(row);
    }
    return true;
}

bool SyncStore::enqueueOperation(const QJsonObject &op)
{
    Row row;
    row.opId = op.value(QStringLiteral("op_id")).toString();
    row.type = op.value(QStringLiteral("type")).toString();
    row.taskUuid = op.value(QStringLiteral("task_id")).toString();
    row.settingKey = op.value(QStringLiteral("key")).toString();
    row.baseRev = intOrNull(op.value(QStringLiteral("base_rev")));
    row.afterOp = op.value(QStringLiteral("after")).toString();
    row.inFlight = op.value(QStringLiteral("in_flight")).toBool();
    for (const QJsonValue &group : op.value(QStringLiteral("changed")).toArray())
        row.changed << group.toString();
    row.changed.sort();
    row.task = op.value(QStringLiteral("task")).toObject();
    row.reason = op.value(QStringLiteral("reason")).toString();
    row.restoreOf = op.value(QStringLiteral("restore_of")).toString();
    row.authoredRev = op.contains(QStringLiteral("authored_rev"))
        ? intOrNull(op.value(QStringLiteral("authored_rev")))
        : (row.type == QLatin1String("update") ? row.baseRev : nullInt());
    if (row.opId.isEmpty() || row.type.isEmpty())
        return fail(QStringLiteral("An operation needs an op_id and a type."));
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());
    if (!writeOperation(row))
        return false;
    return tx.commit() || fail(tx.error());
}

// ---- batches -------------------------------------------------------------

bool SyncStore::reopenRestoreRecord(const QString &recordId)
{
    QString error;
    if (recordId.isEmpty())
        return true;
    if (!runSql(db(), QStringLiteral("UPDATE sync_conflicts SET state = 'open', restore_op_id = NULL, "
                                     "resolved_at = NULL WHERE id = ? AND state = 'restored'"),
                {recordId}, &error))
        return fail(error);
    return true;
}

bool SyncStore::prepareBatch(QList<QByteArray> *payloads, int limit)
{
    payloads->clear();
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());

    // 11.4: compaction, for tasks without anything in flight.
    QStringList taskIds;
    for (const Row &row : allRows()) {
        if (!row.taskUuid.isEmpty() && !taskIds.contains(row.taskUuid))
            taskIds << row.taskUuid;
    }
    QString error;
    for (const QString &uuid : taskIds) {
        const QList<Row> rows = rowsForTask(uuid);
        bool inFlight = false;
        int lastDelete = -1;
        for (int i = 0; i < rows.size(); ++i) {
            inFlight = inFlight || rows.at(i).inFlight;
            if (rows.at(i).type == QLatin1String("delete"))
                lastDelete = i;
        }
        if (inFlight || lastDelete < 0)
            continue;
        const bool neverSent = rows.first().type == QLatin1String("create");
        for (int i = 0; i < rows.size(); ++i) {
            const Row &row = rows.at(i);
            const bool drop = neverSent ? true : (i < lastDelete && row.type == QLatin1String("update"));
            if (!drop)
                continue;
            if (!deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf))
                return false;
        }
        if (neverSent
            && !runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error))
            return fail(error);
    }

    // In-flight rows first, byte-identical; then rows whose base is resolved.
    QList<Row> fresh;
    for (const Row &row : allRows()) {
        if (row.inFlight) {
            payloads->append(row.sentPayload.isEmpty() ? compactJson(wirePayload(row)) : row.sentPayload.toUtf8());
        } else if (row.afterOp.isEmpty() && (!row.baseRev.isNull() || row.type == QLatin1String("create")
                                             || row.type == QLatin1String("set_setting"))) {
            if (payloads->size() + fresh.size() < limit)
                fresh << row;
        }
    }
    for (Row row : fresh) {
        const QByteArray payload = compactJson(wirePayload(row));
        row.inFlight = true;
        row.sentPayload = QString::fromUtf8(payload);
        if (!updateRow(row))
            return false;
        payloads->append(payload);
    }
    if (!tx.commit())
        return fail(tx.error());
    return true;
}

// ---- pull ----------------------------------------------------------------

bool SyncStore::applyPullPage(const QJsonArray &items, const QJsonArray &conflicts, const QVariant &cursor)
{
    return applyPullPageImpl(items, conflicts, cursor, nullptr);
}

SyncStore::PullOutcome SyncStore::applyPullPage(const QJsonArray &items, const QJsonArray &conflicts,
                                                const QVariant &cursor, const QString &serverEpoch)
{
    if (!isLinked()) {
        fail(QStringLiteral("Sync is not linked."));
        return PullOutcome::NotLinked;
    }
    // Like a push answer, a page without an epoch is malformed and applies nothing.
    if (serverEpoch.isEmpty()) {
        fail(QStringLiteral("The pull page carries no server epoch."));
        return PullOutcome::Failed;
    }
    PullOutcome outcome = PullOutcome::Failed;
    applyPullPageImpl(items, conflicts, cursor, &serverEpoch, &outcome);
    return outcome;
}

bool SyncStore::applyPullPageImpl(const QJsonArray &items, const QJsonArray &conflicts, const QVariant &cursor,
                                  const QString *serverEpoch, PullOutcome *outcome)
{
    if (outcome)
        *outcome = PullOutcome::Failed;
    // A page that arrives after unlink must not touch tasks edited while unlinked.
    if (!isLinked()) {
        if (outcome)
            *outcome = PullOutcome::NotLinked;
        return fail(QStringLiteral("Sync is not linked."));
    }
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());
    if (serverEpoch) {
        // Read inside the transaction. A page from another epoch applies nothing (SYNC 10);
        // the first epoch seen is remembered together with the page, as for a push answer.
        const QString stored = stateValue(QStringLiteral("epoch"));
        if (!stored.isEmpty() && stored != *serverEpoch) {
            if (outcome)
                *outcome = PullOutcome::EpochChanged;
            return fail(QStringLiteral("The server epoch changed."));
        }
        if (stored.isEmpty() && !setStateValue(QStringLiteral("epoch"), *serverEpoch))
            return false;
    }
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QString kind = item.value(QStringLiteral("kind")).toString();
        bool ok = true;
        if (kind == QLatin1String("task"))
            ok = applyTaskItem(item);
        else if (kind == QLatin1String("tombstone"))
            ok = applyTombstoneItem(item);
        else if (kind == QLatin1String("setting")) {
            const QString key = item.value(QStringLiteral("key")).toString();
            const int rev = item.value(QStringLiteral("rev")).toInt();
            // Forward only: an older value never replaces a newer one.
            if (rev > stateValue(QStringLiteral("setting_rev.") + key, QStringLiteral("0")).toInt()
                || !item.contains(QStringLiteral("rev")))
                ok = setStateValue(QStringLiteral("setting.") + key, item.value(QStringLiteral("value")).toString())
                    && setStateValue(QStringLiteral("setting_rev.") + key, QString::number(rev));
        }
        if (!ok)
            return false;
    }
    if (!storeServerConflicts(conflicts))
        return false;
    if (cursor.isValid() && !cursor.isNull() && !setStateValue(QStringLiteral("cursor"), cursor.toString()))
        return false;
    if (!tx.commit())
        return fail(tx.error());
    if (outcome)
        *outcome = PullOutcome::Applied;
    return true;
}

bool SyncStore::applyTaskItem(const QJsonObject &item)
{
    const QString uuid = item.value(QStringLiteral("task_id")).toString();
    const int rev = item.value(QStringLiteral("rev")).toInt();
    const QJsonObject server = normalizeTask(item.value(QStringLiteral("task")).toObject(), uuid);
    QString error;

    TaskInfo info;
    if (!taskInfo(uuid, &info)) {
        for (const Row &row : rowsForTask(uuid)) {
            if (row.type == QLatin1String("delete"))
                return true; // our own delete decides; a stale one answers edit_won
        }
        // A live copy older than a tombstone we already know is stale (forward only).
        QSqlQuery known;
        if (selectSql(db(), &known, QStringLiteral("SELECT tombstone_rev FROM sync_tombstones WHERE task_uuid = ?"),
                      {uuid}, &error)
            && known.next() && !known.value(0).isNull() && known.value(0).toInt() >= rev)
            return true;
        if (!insertTask(server, rev) || !storeBase(uuid, rev, server))
            return false;
        if (!runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error))
            return fail(error);
        return true;
    }
    if (!info.serverRev.isNull() && info.serverRev.toInt() >= rev)
        return true; // older than what we hold, or the echo of our own push

    if (!info.held.isEmpty()) {
        // A copy not newer than the recorded tombstone predates the deletion.
        if (!info.heldRev.isNull() && rev <= info.heldRev.toInt())
            return true;
        // Live again elsewhere: the hold ends and the task is reconciled (9.2). A Keep
        // that was not sent yet is moot now; its content is carried by the reconciliation.
        for (const Row &row : rowsForTask(uuid)) {
            if (row.type == QLatin1String("restore") && !row.inFlight
                && (!deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf)))
                return false;
        }
        if (!runSql(db(), QStringLiteral("UPDATE tasks SET held = NULL, held_rev = NULL, held_deleted_at = NULL "
                                         "WHERE uuid = ?"),
                    {uuid}, &error))
            return fail(error);
        return reconcileInTx(item);
    }

    QJsonObject local;
    loadTask(uuid, &local);
    QJsonObject base;
    const bool haveBase = loadBase(uuid, &base);
    QList<Row> rows = rowsForTask(uuid);

    auto collides = [&](const QStringList &changed) {
        for (const QString &group : changed) {
            const QJsonValue theirs = groupValue(server, group);
            if (theirs == groupValue(local, group))
                continue;
            if (haveBase && theirs == groupValue(base, group))
                continue;
            return true;
        }
        return false;
    };

    // A restore row decided against older cloud data is cancelled, not sent (11.12).
    for (const Row &row : rows) {
        if (row.inFlight || row.restoreOf.isEmpty() || row.type != QLatin1String("update") || !collides(row.changed))
            continue;
        if (!deleteRow(row.opId))
            return false;
        const QString group = row.changed.value(0);
        if (!runSql(db(),
                    QStringLiteral("UPDATE sync_conflicts SET state = 'open', restore_op_id = NULL, "
                                   "resolved_at = NULL, theirs_value = ?, server_rev_seen = ? "
                                   "WHERE id = ? AND state = 'restored'"),
                    {encodeValue(groupValue(server, group)), rev, row.restoreOf}, &error))
            return fail(error);
    }
    rows = rowsForTask(uuid);

    // Groups the user changed and the server has not yet been told about.
    QStringList pending;
    for (const Row &row : rows) {
        if (row.type == QLatin1String("update"))
            pending << row.changed;
        else if (row.type == QLatin1String("create") || row.type == QLatin1String("restore"))
            pending << groups();
    }
    // A refused task waits for a deliberate edit, so its differences from the base count as
    // waiting. A distrusted base is the exception: this server copy is what 9.2 reconciles
    // against, so those differences are recomputed below and sent where only this device changed them.
    const bool distrusted = haveBase && !baseTrusted(uuid);
    if (!info.notSynced.isEmpty() && haveBase && !distrusted)
        pending << differingGroups(base, local);
    pending.removeDuplicates();

    QJsonObject merged = server;
    for (const QString &group : pending)
        setGroup(merged, group, groupValue(local, group));

    // Local values that no waiting operation carries (an edit made while unlinked) must
    // not be replaced by the server copy unseen: reconcile them by 9.2.
    QStringList unprotected;
    if (haveBase) {
        for (const QString &group : differingGroups(base, local)) {
            if (!pending.contains(group))
                unprotected << group;
        }
    } else if (rows.isEmpty()) {
        unprotected = differingGroups(server, local);
    }
    QStringList carried;
    if (!unprotected.isEmpty() && !reconcileGroups(uuid, local, server, rev, unprotected, &merged, &carried))
        return false;
    if (distrusted && !info.notSynced.isEmpty()
        && !runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = NULL WHERE uuid = ?"), {uuid}, &error))
        return fail(error);

    for (Row row : rows) {
        if (row.inFlight || row.type != QLatin1String("update"))
            continue;
        const bool collision = collides(row.changed);
        row.task = merged;
        if (!row.afterOp.isEmpty()) {
            row.collided = row.collided || collision;
        } else if (!collision) {
            row.baseRev = rev;
        }
        if (!updateRow(row))
            return false;
    }

    if (merged != local && !writeTaskFields(uuid, merged))
        return false;
    if (!storeBase(uuid, rev, server))
        return false;
    if (carried.isEmpty())
        return true;

    Row row;
    row.opId = newOpId();
    row.type = QStringLiteral("update");
    row.taskUuid = uuid;
    row.baseRev = rev;
    row.authoredRev = rev;
    row.changed = carried;
    row.task = merged;
    return writeOperation(row);
}

bool SyncStore::applyTombstoneItem(const QJsonObject &item)
{
    const QString uuid = item.value(QStringLiteral("task_id")).toString();
    const int rev = item.value(QStringLiteral("rev")).toInt();
    const QString deletedAt = item.value(QStringLiteral("deleted_at")).toString();
    QString error;

    TaskInfo info;
    const bool exists = taskInfo(uuid, &info);
    // A tombstone is always newer than the live copy it ends; one that is not is stale.
    if (exists && info.held.isEmpty() && !info.serverRev.isNull() && rev <= info.serverRev.toInt())
        return true;
    const QList<Row> rows = rowsForTask(uuid);
    bool pendingDelete = false;
    bool keepsEdit = false;
    for (const Row &row : rows) {
        if (row.type == QLatin1String("delete") && !row.inFlight)
            pendingDelete = true;
        else if (row.type == QLatin1String("update") || row.type == QLatin1String("create")
                 || row.type == QLatin1String("restore"))
            keepsEdit = true;
    }

    if (pendingDelete) { // our delete is satisfied
        for (const Row &row : rows) {
            if (!row.inFlight && (!deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf)))
                return false;
        }
        if (!deleteTaskRows(uuid)
            || !runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error))
            return error.isEmpty() ? false : fail(error);
        return true;
    }
    if (!exists) {
        if (!runSql(db(),
                    QStringLiteral("UPDATE sync_tombstones SET tombstone_rev = ? WHERE task_uuid = ? "
                                   "AND (tombstone_rev IS NULL OR tombstone_rev < ?)"),
                    {rev, uuid, rev}, &error))
            return fail(error);
        return true;
    }
    if (!info.held.isEmpty())
        return setHeld(uuid, rev, deletedAt);
    if (keepsEdit)
        return true; // edit wins: the queued operation resurrects the task (7.3)
    // Unsent content must not vanish silently: a task the server never gave us, or one whose
    // operation was refused, is held for the user to decide instead of being removed.
    if (!info.notSynced.isEmpty() || info.serverRev.isNull())
        return setHeld(uuid, rev, deletedAt);
    return deleteTaskRows(uuid);
}

// ---- conflicts delivered by the server -------------------------------------

bool SyncStore::storeServerConflicts(const QJsonArray &conflicts)
{
    const QString me = stateValue(QStringLiteral("device_id"));
    for (const QJsonValue &value : conflicts) {
        const QJsonObject record = value.toObject();
        const QString kind = record.value(QStringLiteral("kind")).toString();
        const QString losing = record.value(QStringLiteral("losing_device_id")).toString();
        const QString winning = record.value(QStringLiteral("winning_device_id")).toString();
        bool recipient = true;
        if (!me.isEmpty()) {
            if (kind == QLatin1String("edit_over_delete"))
                recipient = winning != me; // everyone but the editor
            else
                recipient = losing == me; // field, setting, delete_lost_to_edit
        }
        if (!recipient)
            continue;

        QJsonObject local;
        local.insert(QStringLiteral("record_uid"), record.value(QStringLiteral("id")));
        local.insert(QStringLiteral("source"), QStringLiteral("server"));
        local.insert(QStringLiteral("kind"), kind);
        local.insert(QStringLiteral("task_id"), record.value(QStringLiteral("task_id")));
        local.insert(QStringLiteral("setting_key"), record.value(QStringLiteral("key")));
        local.insert(QStringLiteral("field_group"), record.value(QStringLiteral("field")));
        if (record.contains(QStringLiteral("losing_value")))
            local.insert(QStringLiteral("mine_value"), record.value(QStringLiteral("losing_value")));
        if (record.contains(QStringLiteral("winning_value")))
            local.insert(QStringLiteral("theirs_value"), record.value(QStringLiteral("winning_value")));
        local.insert(QStringLiteral("other_device"), kind == QLatin1String("field") || kind == QLatin1String("setting")
                                                          ? winning : (kind == QLatin1String("edit_over_delete") ? winning : QString()));
        local.insert(QStringLiteral("change_seq"), record.value(QStringLiteral("change_seq")));
        local.insert(QStringLiteral("server_rev_seen"), record.value(QStringLiteral("change_seq")));
        local.insert(QStringLiteral("detected_at"), record.value(QStringLiteral("detected_at")));
        local.insert(QStringLiteral("delete_reason"), record.value(QStringLiteral("delete_reason")));
        local.insert(QStringLiteral("deleted_at"), record.value(QStringLiteral("deleted_at")));
        if (!insertConflict(local))
            return false;
    }
    return true;
}

bool SyncStore::insertConflict(const QJsonObject &record, bool *inserted)
{
    auto text = [&](const char *key) {
        const QJsonValue value = record.value(QLatin1String(key));
        return value.isString() && !value.toString().isEmpty() ? QVariant(value.toString()) : nullText();
    };
    auto valueOf = [&](const char *key) {
        const QString name = QLatin1String(key);
        return record.contains(name) ? encodeValue(record.value(name)) : nullText();
    };
    const QJsonValue seen = record.value(QStringLiteral("server_rev_seen"));
    const QString id = record.value(QStringLiteral("id")).toString();
    const QString state = record.value(QStringLiteral("state")).toString();
    QString error;
    int affected = 0;
    if (!runSql(db(),
                QStringLiteral("INSERT OR IGNORE INTO sync_conflicts (id, record_uid, source, kind, task_uuid, "
                               "setting_key, field_group, mine_value, theirs_value, base_value, server_rev_seen, "
                               "other_device, detected_at, change_seq, delete_reason, deleted_at, state, "
                               "restore_op_id, resolved_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                               "?, ?, ?, ?)"),
                {id.isEmpty() ? newUuid() : id, text("record_uid"), record.value(QStringLiteral("source")).toString(),
                 record.value(QStringLiteral("kind")).toString(), text("task_id"), text("setting_key"),
                 text("field_group"), valueOf("mine_value"), valueOf("theirs_value"), valueOf("base_value"),
                 seen.isNull() || seen.isUndefined() ? nullInt() : QVariant(static_cast<qlonglong>(seen.toInt())),
                 text("other_device"), text("detected_at").isNull() ? QVariant(nowText()) : text("detected_at"),
                 intOrNull(record.value(QStringLiteral("change_seq"))), text("delete_reason"), text("deleted_at"),
                 state.isEmpty() ? QStringLiteral("open") : state, text("restore_op_id"), text("resolved_at")},
                &error, &affected))
        return fail(error);
    if (inserted)
        *inserted = affected > 0;
    return true;
}

bool SyncStore::storeConflict(const QJsonObject &record)
{
    return insertConflict(record);
}

// ---- push acknowledgement ------------------------------------------------

SyncStore::AckOutcome SyncStore::applyPushResponse(const QJsonObject &response, qint64 requestLinkGeneration,
                                                   const AckFault &fault)
{
    QList<Row> inFlight;
    for (const Row &row : allRows()) {
        if (row.inFlight)
            inFlight << row;
    }

    const QJsonValue resultsValue = response.value(QStringLiteral("results"));
    const QJsonArray results = resultsValue.toArray();
    // An empty epoch is malformed: it must never be remembered as the initial epoch.
    bool wellFormed = resultsValue.isArray() && response.value(QStringLiteral("server_epoch")).isString()
        && !response.value(QStringLiteral("server_epoch")).toString().isEmpty();
    QStringList resultIds;
    for (const QJsonValue &value : results) {
        const QJsonObject result = value.toObject();
        if (!value.isObject() || !result.value(QStringLiteral("op_id")).isString())
            wellFormed = false;
        resultIds << result.value(QStringLiteral("op_id")).toString();
    }
    if (!wellFormed)
        return AckOutcome::UnknownOutcome;

    // A duplicate delivery of an answer that was already applied.
    bool anyMatch = false;
    for (const Row &row : inFlight)
        anyMatch = anyMatch || resultIds.contains(row.opId);
    if (inFlight.isEmpty() || (!anyMatch && !resultIds.isEmpty()))
        return AckOutcome::Ignored;

    QStringList expected;
    for (const Row &row : inFlight)
        expected << row.opId;
    if (expected != resultIds)
        return AckOutcome::UnknownOutcome;
    // Every result is validated before any state changes (11.11, step 1): an unknown or
    // missing status, or a missing required revision or task, makes the outcome unknown.
    for (int i = 0; i < results.size(); ++i) {
        if (!resultIsValid(inFlight.at(i), results.at(i).toObject()))
            return AckOutcome::UnknownOutcome;
    }

    const QString storedEpoch = stateValue(QStringLiteral("epoch"));
    if (!storedEpoch.isEmpty() && storedEpoch != response.value(QStringLiteral("server_epoch")).toString())
        return AckOutcome::EpochChanged;
    auto generation = [this] { return stateValue(QStringLiteral("link_generation"), QStringLiteral("0")).toLongLong(); };
    if (generation() != requestLinkGeneration)
        return AckOutcome::Dropped;
    if (fault.beforeApply)
        return AckOutcome::CrashedBeforeApply;

    DbTransaction tx(db());
    if (!tx.begin()) {
        fail(tx.error());
        return AckOutcome::RolledBack;
    }
    if (generation() != requestLinkGeneration) // re-read inside the transaction
        return AckOutcome::Dropped;
    // The first epoch seen is remembered in the same transaction as the results,
    // before any base is stored under it, so later responses can be compared (SYNC 10).
    if (storedEpoch.isEmpty()
        && !setStateValue(QStringLiteral("epoch"), response.value(QStringLiteral("server_epoch")).toString()))
        return AckOutcome::RolledBack;

    for (int i = 0; i < results.size(); ++i) {
        if (fault.duringResult == i + 1)
            return AckOutcome::RolledBack; // ~DbTransaction rolls back every result applied so far
        if (!applyResult(inFlight.at(i), results.at(i).toObject()))
            return AckOutcome::RolledBack;
    }
    if (fault.beforeCommit)
        return AckOutcome::RolledBack;
    if (!tx.commit()) {
        fail(tx.error());
        return AckOutcome::RolledBack;
    }
    return fault.afterCommit ? AckOutcome::CommittedThenCrashed : AckOutcome::Committed;
}

bool SyncStore::resultIsValid(const Row &row, const QJsonObject &result) const
{
    const QJsonValue statusValue = result.value(QStringLiteral("status"));
    if (!statusValue.isString())
        return false;
    const QString status = statusValue.toString();
    const QJsonValue rev = result.value(QStringLiteral("rev"));
    if (status == QLatin1String("rejected")) {
        const QString code = result.value(QStringLiteral("code")).toString();
        if (code.isEmpty())
            return false;
        if (code == QLatin1String("already_deleted") && !wholeRev(rev))
            return false;
        if ((code == QLatin1String("not_deleted") || code == QLatin1String("edit_won"))
            && (!wholeRev(rev) || !result.value(QStringLiteral("task")).isObject()))
            return false;
        return true;
    }
    static const QStringList accepted = {QStringLiteral("applied"), QStringLiteral("merged"),
                                         QStringLiteral("conflict_resolved"), QStringLiteral("resurrected"),
                                         QStringLiteral("restored"), QStringLiteral("noop")};
    if (!accepted.contains(status) || !wholeRev(rev))
        return false;
    // The canonical task is what the base is built from; without it nothing can be adopted.
    const bool carriesTask = row.type == QLatin1String("create") || row.type == QLatin1String("update")
        || row.type == QLatin1String("restore");
    return !carriesTask || result.value(QStringLiteral("task")).isObject();
}

bool SyncStore::dropCollidedRestores(const Row &acked, const QJsonObject &server, int rev, const QJsonObject &oldBase,
                                     bool haveOldBase, const QStringList &ackedGroups)
{
    QString error;
    for (Row row : rowsForTask(acked.taskUuid)) {
        if (row.afterOp != acked.opId || row.restoreOf.isEmpty() || row.type != QLatin1String("update"))
            continue;
        QStringList collided;
        for (const QString &group : row.changed) {
            if (ackedGroups.contains(group))
                continue;
            const QJsonValue theirs = groupValue(server, group);
            if (theirs == groupValue(row.task, group) || (haveOldBase && theirs == groupValue(oldBase, group)))
                continue;
            collided << group;
        }
        if (collided.isEmpty())
            continue;
        for (const QString &group : collided)
            row.changed.removeAll(group);
        if (!(row.changed.isEmpty() ? deleteRow(row.opId) : updateRow(row)))
            return false;
        // Back to open with the newer cloud value; the restore is never sent (11.12).
        if (!runSql(db(),
                    QStringLiteral("UPDATE sync_conflicts SET state = 'open', restore_op_id = NULL, "
                                   "resolved_at = NULL, theirs_value = ?, server_rev_seen = ? "
                                   "WHERE id = ? AND state = 'restored'"),
                    {encodeValue(groupValue(server, collided.first())), rev, row.restoreOf}, &error))
            return fail(error);
    }
    return true;
}

// Drops `opId` and every row that waits behind it (transitively).
bool SyncStore::dropRowsOfTask(const QString &uuid, const QString &afterOp)
{
    QStringList dropping{afterOp};
    bool grew = true;
    const QList<Row> rows = rowsForTask(uuid);
    QList<Row> removed;
    while (grew) {
        grew = false;
        for (const Row &row : rows) {
            if (!row.afterOp.isEmpty() && dropping.contains(row.afterOp) && !dropping.contains(row.opId)) {
                dropping << row.opId;
                grew = true;
            }
        }
    }
    for (const Row &row : rows) {
        if (dropping.contains(row.opId) && (!deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf)))
            return false;
    }
    return true;
}

bool SyncStore::resolveDependents(const Row &acked, const QJsonObject &server, const QJsonObject &oldBase,
                                  bool haveOldBase, int storedRev, const QStringList &ackedGroups)
{
    QJsonObject local;
    const bool haveLocal = loadTask(acked.taskUuid, &local);
    for (Row row : rowsForTask(acked.taskUuid)) {
        if (row.afterOp != acked.opId)
            continue;
        row.afterOp.clear();
        bool collision = false;
        if (row.type == QLatin1String("update")) {
            for (const QString &group : row.changed) {
                if (ackedGroups.contains(group))
                    continue;
                const QJsonValue theirs = groupValue(server, group);
                if (theirs == groupValue(row.task, group) || (haveOldBase && theirs == groupValue(oldBase, group)))
                    continue;
                collision = true;
            }
            if (haveLocal)
                row.task = local;
        }
        if (row.type == QLatin1String("restore") && row.task.isEmpty() && haveLocal)
            row.task = local;
        row.baseRev = (collision || row.collided) && !row.authoredRev.isNull() ? row.authoredRev
                                                                                : QVariant(storedRev);
        if (!updateRow(row))
            return false;
    }
    return true;
}

bool SyncStore::applyResult(const Row &row, const QJsonObject &result)
{
    const QString uuid = row.taskUuid;
    const QString status = result.value(QStringLiteral("status")).toString();
    const QString code = result.value(QStringLiteral("code")).toString();
    const int rev = result.value(QStringLiteral("rev")).toInt(-1);
    QString error;

    if (!storeServerConflicts(result.value(QStringLiteral("conflicts")).toArray()))
        return false;

    if (status == QLatin1String("rejected")) {
        TaskInfo info;
        const bool exists = taskInfo(uuid, &info);
        const QJsonObject returned = normalizeTask(result.value(QStringLiteral("task")).toObject(), uuid);
        if (code == QLatin1String("invalid_base_rev") && row.type == QLatin1String("restore")) {
            // A restore is never retried (4.3, 11.8): the tombstone the user decided about is
            // not the current one, so the decision is void and nothing can refresh it here.
            if (exists && !info.held.isEmpty()) {
                // Cancelled with the rows behind it; the task stays held with its content,
                // edits included. The recorded tombstone revision is out of date, so it is
                // cleared: Keep is refused until a pull (holdRestoreRejected, or a pulled
                // tombstone/task) says what the server holds now and the user decides again.
                return dropRowsOfTask(uuid, row.opId) && deleteRow(row.opId) && reopenRestoreRecord(row.restoreOf)
                    && runSql(db(), QStringLiteral("UPDATE tasks SET held_rev = NULL WHERE uuid = ?"), {uuid}, &error);
            }
            if (!exists) {
                // The user deleted the task while the Keep was in flight. The Keep is void; the
                // delete behind it stays, based on the last copy this device saw (a tombstone
                // makes it a no-op, a newer live copy answers edit_won: 7.2, 7.4).
                QSqlQuery record;
                QVariant seen = nullInt();
                if (selectSql(db(), &record, QStringLiteral("SELECT server_rev FROM sync_tombstones WHERE task_uuid = ?"),
                              {uuid}, &error)
                    && record.next() && !record.value(0).isNull())
                    seen = record.value(0).toLongLong();
                for (Row other : rowsForTask(uuid)) {
                    if (other.afterOp != row.opId)
                        continue;
                    if (other.type == QLatin1String("delete") && !seen.isNull()) {
                        other.afterOp.clear();
                        other.baseRev = seen;
                        if (!updateRow(other))
                            return false;
                    } else if (!dropRowsOfTask(uuid, other.opId)) {
                        return false;
                    }
                }
                return deleteRow(row.opId) && reopenRestoreRecord(row.restoreOf);
            }
            // Not held any more (the task is live here): reconcile it (9.2), nothing is retried.
            if (!dropRowsOfTask(uuid, row.opId) || !deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf))
                return false;
            return runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = 'needs_reconcile' WHERE uuid = ?"),
                          {uuid}, &error)
                || fail(error);
        }
        if (code == QLatin1String("invalid_base_rev")) {
            // The rejection is final for this op_id: the server stored it and would replay it
            // (4.0). The outcome is known, so one retry goes out under a NEW op_id; its
            // payload is rebuilt from the row, so a pull's rebase (11.5) has refreshed the
            // base_rev and task by then. Position in the outbox (seq) and the rows waiting
            // behind it are kept. A second rejection stops the retries and reconciles (9.2).
            if (row.attempts < 1) {
                const QString fresh = newOpId();
                if (!runSql(db(),
                            QStringLiteral("UPDATE sync_outbox SET op_id = ?, state = 'pending', sent_payload = NULL, "
                                           "attempts = attempts + 1 WHERE op_id = ?"),
                            {fresh, row.opId}, &error)
                    || !runSql(db(), QStringLiteral("UPDATE sync_outbox SET after_op_id = ? WHERE after_op_id = ?"),
                               {fresh, row.opId}, &error)
                    || !runSql(db(), QStringLiteral("UPDATE sync_conflicts SET restore_op_id = ? WHERE restore_op_id = ?"),
                               {fresh, row.opId}, &error))
                    return fail(error);
                return true;
            }
            if (!dropRowsOfTask(uuid, row.opId) || !reopenRestoreRecord(row.restoreOf))
                return false;
            if (!exists)
                return true; // a delete: nothing local to reconcile, the next pull decides
            return runSql(db(), QStringLiteral("UPDATE sync_base SET trusted = 0 WHERE task_uuid = ?"), {uuid}, &error)
                && runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = 'needs_reconcile' WHERE uuid = ?"),
                          {uuid}, &error)
                || fail(error);
        }
        if (code == QLatin1String("unknown_task")) {
            // The server has no copy: send a create under a new op id (9.2).
            QJsonObject local;
            if (!exists || !loadTask(uuid, &local)) {
                // The task is gone here and the server never had it: neither its tombstone record
                // nor its base can be used by anything any more.
                if (!dropRowsOfTask(uuid, row.opId) || !deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf))
                    return false;
                return (runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error)
                        && runSql(db(), QStringLiteral("DELETE FROM sync_base WHERE task_uuid = ?"), {uuid}, &error))
                    || fail(error);
            }
            if (!dropRowsOfTask(uuid, row.opId) || !deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf)
                || !setServerRev(uuid, nullInt())
                || !runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error)
                || !runSql(db(), QStringLiteral("UPDATE sync_base SET trusted = 0 WHERE task_uuid = ?"), {uuid},
                           &error))
                return error.isEmpty() ? false : fail(error);
            Row create;
            create.opId = newOpId();
            create.type = QStringLiteral("create");
            create.taskUuid = uuid;
            create.task = local;
            return insertRow(create);
        }
        if (code == QLatin1String("not_deleted") && !exists) {
            // The user deleted the task after the restore was sent and the server has it live
            // anyway: the delete that waits behind the restore is based on this answer.
            bool deleteWaiting = false;
            for (const Row &other : rowsForTask(uuid))
                deleteWaiting = deleteWaiting || (other.afterOp == row.opId && other.type == QLatin1String("delete"));
            if (deleteWaiting) {
                if (!storeBase(uuid, rev, returned) || !resolveDependents(row, returned, QJsonObject(), false, rev, {})
                    || !reopenRestoreRecord(row.restoreOf))
                    return false;
                return deleteRow(row.opId);
            }
        }
        if (!dropRowsOfTask(uuid, row.opId) || !deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf))
            return false;
        const QJsonObject returnedItem{{QStringLiteral("kind"), QStringLiteral("task")},
                                       {QStringLiteral("task_id"), uuid},
                                       {QStringLiteral("rev"), rev},
                                       {QStringLiteral("task"), returned}};
        if (code == QLatin1String("edit_won") || code == QLatin1String("not_deleted")) {
            if (!exists)
                return replaceWithServerCopy(uuid, returned, rev);
            // The local row holds content the server has not seen (an undone or kept task):
            // adopt the returned task as the base but reconcile by 9.2, so nothing local is
            // overwritten unseen. Differences are carried or preserved in a record.
            if (!runSql(db(), QStringLiteral("UPDATE tasks SET held = NULL, held_rev = NULL, held_deleted_at = NULL, "
                                             "not_synced = NULL WHERE uuid = ?"),
                        {uuid}, &error)
                || !runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error))
                return fail(error);
            return reconcileInTx(returnedItem);
        }
        if (!exists) {
            // Nothing local to protect; drop what only a task that is gone could use.
            if (!runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error)
                || !runSql(db(), QStringLiteral("DELETE FROM sync_base WHERE task_uuid = ?"), {uuid}, &error))
                return fail(error);
            return true;
        }
        if (code == QLatin1String("already_exists") && wholeRev(result.value(QStringLiteral("rev")))
            && result.value(QStringLiteral("task")).isObject()) {
            // 9.2: the id exists on the server with other content; reconcile, never overwrite.
            if (!runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = NULL WHERE uuid = ?"), {uuid}, &error))
                return fail(error);
            return reconcileInTx(returnedItem);
        }
        if (code == QLatin1String("already_deleted"))
            return setHeld(uuid, rev, QString());
        const QString reason = code == QLatin1String("validation_failed") || code == QLatin1String("empty_changes")
            ? code : QStringLiteral("needs_reconcile");
        if (!runSql(db(), QStringLiteral("UPDATE tasks SET not_synced = ? WHERE uuid = ?"), {reason, uuid}, &error))
            return fail(error);
        return true;
    }

    // ---- accepted: applied, merged, conflict_resolved, resurrected, restored, noop
    if (row.type == QLatin1String("set_setting")) {
        // Forward only: a replayed older answer never lowers the recorded revision.
        const QString key = QStringLiteral("setting_rev.") + row.settingKey;
        if (rev > stateValue(key, QStringLiteral("0")).toInt() && !setStateValue(key, QString::number(rev)))
            return false;
        return deleteRow(row.opId);
    }

    if (row.type == QLatin1String("delete")) {
        // The answer carries the tombstone's revision; a waiting restore is based on it.
        if (!runSql(db(),
                    QStringLiteral("UPDATE sync_tombstones SET tombstone_rev = ? WHERE task_uuid = ? "
                                   "AND (tombstone_rev IS NULL OR tombstone_rev < ?)"),
                    {rev, uuid, rev}, &error))
            return fail(error);
        bool restoreWaiting = false;
        const QList<Row> rows = rowsForTask(uuid);
        for (const Row &other : rows)
            restoreWaiting = restoreWaiting || (other.afterOp == row.opId && other.type == QLatin1String("restore"));
        if (!resolveDependents(row, QJsonObject(), QJsonObject(), false, rev, {}))
            return false;
        if (!restoreWaiting
            && !runSql(db(), QStringLiteral("DELETE FROM sync_base WHERE task_uuid = ?"), {uuid}, &error))
            return fail(error);
        return deleteRow(row.opId);
    }

    TaskInfo info;
    const bool exists = taskInfo(uuid, &info);
    const QJsonObject returned = normalizeTask(result.value(QStringLiteral("task")).toObject(), uuid);
    QJsonObject oldBase;
    int baseRev = 0;
    const bool haveOldBase = loadBase(uuid, &oldBase, &baseRev);
    const QVariant stored = exists ? info.serverRev : (haveOldBase ? QVariant(baseRev) : nullInt());
    const bool advance = stored.isNull() || rev > stored.toInt();

    QStringList ackedGroups = row.type == QLatin1String("update") ? row.changed : groups();
    QJsonObject current = haveOldBase ? oldBase : QJsonObject();
    int currentRev = stored.isNull() ? rev : stored.toInt();

    if (advance) {
        // A restore row decided against older cloud data is dropped before anything below
        // counts its group as a waiting local change (11.12).
        if (!dropCollidedRestores(row, returned, rev, oldBase, haveOldBase, ackedGroups))
            return false;
        if (exists) {
            QJsonObject local;
            loadTask(uuid, &local);
            QStringList pending;
            for (const Row &other : rowsForTask(uuid)) {
                if (other.opId == row.opId)
                    continue;
                if (other.type == QLatin1String("update"))
                    pending << other.changed;
                else if (other.type == QLatin1String("create") || other.type == QLatin1String("restore"))
                    pending << groups();
            }
            if (!info.notSynced.isEmpty() && haveOldBase)
                pending << differingGroups(oldBase, local);
            QJsonObject merged = returned;
            for (const QString &group : pending)
                setGroup(merged, group, groupValue(local, group));
            if (merged != local && !writeTaskFields(uuid, merged))
                return false;
        }
        if (exists) {
            if (!storeBase(uuid, rev, returned))
                return false;
        } else if (haveOldBase
                   && !runSql(db(), QStringLiteral("UPDATE sync_base SET rev = ?, snapshot = ? WHERE task_uuid = ?"),
                              {rev, jsonToText(returned), uuid}, &error)) {
            return fail(error);
        }
        current = returned;
        currentRev = rev;
    }

    if (!resolveDependents(row, current, oldBase, haveOldBase, currentRev, ackedGroups))
        return false;
    if (status == QLatin1String("restored") || row.type == QLatin1String("restore")) {
        // A delete queued behind the restore (the user deleted the task meanwhile) keeps its record.
        bool deleteWaiting = false;
        for (const Row &other : rowsForTask(uuid))
            deleteWaiting = deleteWaiting || other.type == QLatin1String("delete");
        if ((!deleteWaiting
             && !runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE task_uuid = ?"), {uuid}, &error))
            || !runSql(db(), QStringLiteral("UPDATE tasks SET held = NULL, held_rev = NULL, held_deleted_at = NULL "
                                            "WHERE uuid = ?"),
                       {uuid}, &error))
            return fail(error);
    }
    return deleteRow(row.opId);
}

// ---- reconciliation (9.2) --------------------------------------------------

bool SyncStore::reconcileTask(const QJsonObject &item, bool failBeforeCommit)
{
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());
    if (!reconcileInTx(item))
        return false;
    if (failBeforeCommit)
        return false; // simulated death: ~DbTransaction rolls back, nothing survives
    if (!tx.commit())
        return fail(tx.error());
    return true;
}

bool SyncStore::reconcileGroups(const QString &uuid, const QJsonObject &local, const QJsonObject &server, int rev,
                                const QStringList &candidates, QJsonObject *merged, QStringList *carried)
{
    QJsonObject base;
    const bool haveBase = loadBase(uuid, &base);
    for (const QString &group : candidates) {
        const QJsonValue l = groupValue(local, group);
        const QJsonValue s = groupValue(server, group);
        if (l == s)
            continue;
        const QJsonValue b = haveBase ? groupValue(base, group) : QJsonValue(QJsonValue::Undefined);
        if (haveBase && l == b) { // the user did not change it: take the server's
            setGroup(*merged, group, s);
        } else if (haveBase && s == b) { // only this device changed it: send it
            setGroup(*merged, group, l);
            *carried << group;
        } else { // both changed, or no base: keep the server's, preserve ours in a record
            QJsonObject record;
            record.insert(QStringLiteral("source"), QStringLiteral("local"));
            record.insert(QStringLiteral("kind"), QStringLiteral("reconcile"));
            record.insert(QStringLiteral("task_id"), uuid);
            record.insert(QStringLiteral("field_group"), group);
            record.insert(QStringLiteral("mine_value"), l);
            record.insert(QStringLiteral("theirs_value"), s);
            if (haveBase)
                record.insert(QStringLiteral("base_value"), b);
            record.insert(QStringLiteral("server_rev_seen"), rev);
            if (!insertConflict(record))
                return false;
            setGroup(*merged, group, s);
        }
    }
    return true;
}

bool SyncStore::reconcileInTx(const QJsonObject &item)
{
    const QString uuid = item.value(QStringLiteral("task_id")).toString();
    const int rev = item.value(QStringLiteral("rev")).toInt();
    const QString kind = item.value(QStringLiteral("kind")).toString();

    QJsonObject local;
    TaskInfo info;
    if (!loadTask(uuid, &local, &info))
        return true; // nothing local to reconcile
    if (kind == QLatin1String("tombstone")) { // never resurrect: hold and ask
        // Held content is kept locally and nothing is queued for it (11.8, 11.6): rows
        // that were not sent are cancelled so that none can resurrect the task later.
        for (const Row &row : rowsForTask(uuid)) {
            if (!row.inFlight && (!deleteRow(row.opId) || !reopenRestoreRecord(row.restoreOf)))
                return false;
        }
        return setHeld(uuid, rev, item.value(QStringLiteral("deleted_at")).toString());
    }

    const QJsonObject server = normalizeTask(item.value(QStringLiteral("task")).toObject(), uuid);
    QJsonObject merged = local;
    QStringList carried;
    if (!reconcileGroups(uuid, local, server, rev, groups(), &merged, &carried))
        return false;
    if (merged != local && !writeTaskFields(uuid, merged))
        return false;
    if (!storeBase(uuid, rev, server))
        return false;
    if (carried.isEmpty())
        return true;

    Row row;
    row.opId = newOpId();
    row.type = QStringLiteral("update");
    row.taskUuid = uuid;
    row.baseRev = rev;
    row.authoredRev = rev;
    row.changed = carried;
    row.task = merged;
    return writeOperation(row);
}

// ---- held tasks ------------------------------------------------------------

bool SyncStore::holdKeep(const QString &uuid)
{
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());
    QJsonObject task;
    TaskInfo info;
    if (!loadTask(uuid, &task, &info) || info.held.isEmpty())
        return fail(QStringLiteral("That task is not held."));
    for (const Row &existing : rowsForTask(uuid)) {
        if (existing.type == QLatin1String("restore"))
            return tx.commit() || fail(tx.error()); // Keep is already waiting: never queue a second restore
    }
    if (info.heldRev.isNull())
        return fail(QStringLiteral("The deletion this decision is about is not known."));
    Row row;
    row.opId = newOpId();
    row.type = QStringLiteral("restore");
    row.taskUuid = uuid;
    row.baseRev = info.heldRev;
    row.task = task;
    if (!writeOperation(row) || !tx.commit())
        return tx.error().isEmpty() ? false : fail(tx.error());
    return true;
}

bool SyncStore::holdDeleteLocal(const QString &uuid)
{
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());
    TaskInfo info;
    if (!taskInfo(uuid, &info) || info.held.isEmpty())
        return fail(QStringLiteral("That task is not held."));
    // A Keep the user already chose must not bring the task back after this (F2).
    if (!discardHeldRows(uuid, info.localId, QStringLiteral("user")) || !deleteTaskRows(uuid) || !tx.commit())
        return tx.error().isEmpty() ? false : fail(tx.error());
    return true;
}

bool SyncStore::holdRestoreRejected(const QString &uuid, bool serverTombstoned, int tombstoneRev, bool *ask,
                                    bool *reconcile)
{
    if (ask)
        *ask = false;
    if (reconcile)
        *reconcile = false;
    if (!serverTombstoned) {
        if (reconcile)
            *reconcile = true;
        return true;
    }
    if (ask)
        *ask = true;
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());
    return setHeld(uuid, tombstoneRev, QString()) && (tx.commit() || fail(tx.error()));
}

// ---- conflict resolution ---------------------------------------------------

bool SyncStore::resolveConflict(const QString &id, const QString &action, const QString &now,
                                ConflictResolution *resolution)
{
    resolution->result = QStringLiteral("unavailable");
    resolution->current = QJsonValue(QJsonValue::Undefined);
    QString error;

    // Everything is decided inside one write transaction: the record's state and the outbox are
    // read under the same lock that the changes below are made under.
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());

    QSqlQuery query;
    if (!selectSql(db(), &query,
                   QStringLiteral("SELECT kind, task_uuid, field_group, mine_value, theirs_value, state, restore_op_id "
                                  "FROM sync_conflicts WHERE id = ?"),
                   {id}, &error))
        return fail(error);
    if (!query.next())
        return true; // unavailable: no such record
    const QString kind = query.value(0).toString();
    const QString uuid = query.value(1).toString();
    const QString group = query.value(2).toString();
    const QJsonValue mine = decodeValue(query.value(3));
    const QJsonValue theirs = decodeValue(query.value(4));
    const QString state = query.value(5).toString();
    const QString restoreOpId = query.value(6).toString();

    // A restore that is queued or in flight owns the record until it is answered: nothing
    // may keep, delete or restore it again meanwhile (it would orphan or duplicate the row).
    // The row is found by its restore_of link or by the operation id the record remembers.
    QSqlQuery waiting;
    if (!selectSql(db(), &waiting,
                   QStringLiteral("SELECT COUNT(*) FROM sync_outbox WHERE restore_of = ? OR op_id = ?"),
                   {id, restoreOpId}, &error))
        return fail(error);
    if (waiting.next() && waiting.value(0).toInt() > 0)
        return true; // unavailable; nothing changed

    if (action == QLatin1String("keep")) {
        if (state == QLatin1String("kept_cloud")) { // already so: keep the original resolution time
            resolution->result = QStringLiteral("kept_cloud");
            return true;
        }
        if (state != QLatin1String("open"))
            return true; // unavailable: a restored record is not "kept" afterwards
        if (!runSql(db(), QStringLiteral("UPDATE sync_conflicts SET state = 'kept_cloud', resolved_at = ? "
                                         "WHERE id = ?"),
                    {now, id}, &error))
            return fail(error);
        resolution->result = QStringLiteral("kept_cloud");
        return tx.commit() || fail(tx.error());
    }
    if (action == QLatin1String("delete")) {
        if (!runSql(db(), QStringLiteral("DELETE FROM sync_conflicts WHERE id = ?"), {id}, &error))
            return fail(error);
        resolution->result = QStringLiteral("deleted");
        return tx.commit() || fail(tx.error());
    }
    if (action != QLatin1String("restore") && action != QLatin1String("restore_confirmed"))
        return fail(QStringLiteral("Unknown conflict action."));

    // Restore my value: only for field-level records, and only as a new user edit.
    QJsonObject task;
    TaskInfo info;
    if ((kind != QLatin1String("field") && kind != QLatin1String("reconcile")) || group.isEmpty()
        || mine.isUndefined() || state == QLatin1String("restored") || !loadTask(uuid, &task, &info)
        || !info.held.isEmpty() || !info.notSynced.isEmpty())
        return true; // unavailable (G1); nothing changed
    const QJsonValue current = groupValue(task, group);
    if (action == QLatin1String("restore")) {
        QJsonObject base;
        const QJsonValue baseValue = loadBase(uuid, &base) ? groupValue(base, group)
                                                           : QJsonValue(QJsonValue::Undefined);
        if (!(current == baseValue && baseValue == theirs)) { // G2
            resolution->result = QStringLiteral("needs_confirm");
            resolution->current = current;
            return true;
        }
    }

    QJsonObject restored = task;
    setGroup(restored, group, mine);
    QString opId;
    if (!writeTaskFields(uuid, restored) || !recordEdit(task, restored, id, &opId))
        return false;
    if (!runSql(db(), QStringLiteral("UPDATE sync_conflicts SET state = 'restored', restore_op_id = ?, "
                                     "resolved_at = ? WHERE id = ?"),
                {opId.isEmpty() ? nullText() : QVariant(opId), now, id}, &error))
        return fail(error);
    resolution->result = QStringLiteral("queued");
    return tx.commit() || fail(tx.error());
}

bool SyncStore::cleanupConflicts(const QString &now)
{
    const QDateTime cutoff = QDateTime::fromString(now, Qt::ISODateWithMs).addDays(-30);
    if (!cutoff.isValid())
        return fail(QStringLiteral("Invalid time."));
    QString error;
    // Open records are never removed: they may hold the only copy of the user's text. A
    // restored record whose restore is still queued or in flight is not finished either: if
    // that row is refused or dropped, the record is reopened and must still exist (11.12).
    if (!runSql(db(), QStringLiteral("DELETE FROM sync_conflicts WHERE state IN ('kept_cloud', 'restored') "
                                     "AND resolved_at IS NOT NULL AND resolved_at <= ? "
                                     "AND NOT EXISTS (SELECT 1 FROM sync_outbox o WHERE o.restore_of = sync_conflicts.id "
                                     "OR o.op_id = sync_conflicts.restore_op_id)"),
                {cutoff.toUTC().toString(Qt::ISODateWithMs)}, &error))
        return fail(error);
    return true;
}

bool SyncStore::cleanupTombstones(const QString &now)
{
    const QDateTime cutoff = QDateTime::fromString(now, Qt::ISODateWithMs).addDays(-30);
    if (!cutoff.isValid())
        return fail(QStringLiteral("Invalid time."));
    const QString before = cutoff.toUTC().toString(Qt::ISODateWithMs);
    QString error;
    DbTransaction tx(db());
    if (!tx.begin())
        return fail(tx.error());
    // A local tombstone record only serves an Undo (seconds) and the guard against a stale live
    // copy being pulled again. Once it is old and nothing queued refers to the task, neither can
    // happen: the server's own tombstone is what other devices see.
    if (!runSql(db(), QStringLiteral("DELETE FROM sync_tombstones WHERE deleted_at <= ? "
                                     "AND NOT EXISTS (SELECT 1 FROM sync_outbox o WHERE o.task_uuid = sync_tombstones.task_uuid) "
                                     "AND NOT EXISTS (SELECT 1 FROM tasks t WHERE t.uuid = sync_tombstones.task_uuid)"),
                {before}, &error)
        // A base nothing refers to any more (no task, no queued operation, no tombstone record).
        || !runSql(db(), QStringLiteral("DELETE FROM sync_base WHERE NOT EXISTS "
                                        "(SELECT 1 FROM tasks t WHERE t.uuid = sync_base.task_uuid) "
                                        "AND NOT EXISTS (SELECT 1 FROM sync_outbox o WHERE o.task_uuid = sync_base.task_uuid) "
                                        "AND NOT EXISTS (SELECT 1 FROM sync_tombstones s WHERE s.task_uuid = sync_base.task_uuid)"),
                   {}, &error))
        return fail(error);
    return tx.commit() || fail(tx.error());
}
