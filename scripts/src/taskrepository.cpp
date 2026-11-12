#include "taskrepository.h"

#include "database.h"
#include "syncstore.h"

#include <QJsonObject>
#include <QSqlDatabase>
#include <QUuid>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariantList>

namespace {

const QString kColumns = QStringLiteral(
    "id, title, notes, list_name, priority, due_at, completed, reminder_at, reminder_sent, "
    "created_at, updated_at, completed_at, uuid");

Task taskFromRow(const QSqlQuery &query)
{
    Task task;
    task.id = query.value(0).toLongLong();
    task.title = query.value(1).toString();
    task.notes = query.value(2).toString();
    task.listName = query.value(3).toString();
    task.priority = query.value(4).toInt();
    task.dueAt = Task::timestampFromVariant(query.value(5));
    task.completed = query.value(6).toInt() != 0;
    task.reminderAt = Task::timestampFromVariant(query.value(7));
    task.reminderSent = query.value(8).toInt() != 0;
    task.createdAt = Task::timestampFromVariant(query.value(9)).value_or(QDateTime());
    task.updatedAt = Task::timestampFromVariant(query.value(10)).value_or(QDateTime());
    task.completedAt = Task::timestampFromVariant(query.value(11));
    task.uuid = query.value(12).toString();
    return task;
}

QVariant nowText()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QJsonValue timestampJson(const std::optional<QDateTime> &timestamp)
{
    const QVariant text = Task::timestampToVariant(timestamp);
    return text.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(text.toString());
}

// The synced fields of a task (api/SYNC.md 2.1).
QJsonObject syncJson(const Task &task)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), task.uuid);
    object.insert(QStringLiteral("title"), task.title);
    object.insert(QStringLiteral("notes"), task.notes);
    object.insert(QStringLiteral("list_name"), task.listName);
    object.insert(QStringLiteral("priority"), task.priority);
    object.insert(QStringLiteral("due_at"), timestampJson(task.dueAt));
    object.insert(QStringLiteral("completed"), task.completed);
    object.insert(QStringLiteral("completed_at"), timestampJson(task.completedAt));
    object.insert(QStringLiteral("reminder_at"), timestampJson(task.reminderAt));
    object.insert(QStringLiteral("created_at"), timestampJson(task.createdAt));
    return object;
}

} // namespace

TaskRepository::TaskRepository(Database &database)
    : m_database(database)
    , m_sync(database)
{
}

bool TaskRepository::fail(const QString &message)
{
    m_lastError = message;
    return false;
}

bool TaskRepository::ensureReady()
{
    m_lastError.clear();
    if (!m_database.isReady())
        return fail(QStringLiteral("The database is not available."));
    return true;
}

bool TaskRepository::validate(Task *task)
{
    task->title = task->title.trimmed();
    if (task->title.isEmpty())
        return fail(QStringLiteral("A task title is required."));
    if (!Task::isValidPriority(task->priority)) {
        return fail(QStringLiteral("Priority must be between %1 and %2.")
                        .arg(Task::kMinPriority)
                        .arg(Task::kMaxPriority));
    }
    // A null QString would be bound as SQL NULL, which `notes` does not allow.
    if (task->notes.isNull())
        task->notes = QStringLiteral("");
    task->listName = task->listName.trimmed();
    if (task->listName.isEmpty())
        task->listName = Task().listName;
    return true;
}

bool TaskRepository::loadTasks(const Query &query, QList<Task> *tasks)
{
    if (!ensureReady())
        return false;

    // The SQL text is built only from constant fragments; every user-supplied
    // value is bound.
    QString sql = QStringLiteral("SELECT %1 FROM tasks").arg(kColumns);
    QStringList conditions;
    QVariantList bindings;

    if (query.id) {
        conditions << QStringLiteral("id = ?");
        bindings << *query.id;
    }
    if (query.completion == CompletionFilter::Active)
        conditions << QStringLiteral("completed = 0");
    else if (query.completion == CompletionFilter::Completed)
        conditions << QStringLiteral("completed = 1");

    if (!query.listName.isEmpty()) {
        conditions << QStringLiteral("list_name = ?");
        bindings << query.listName;
    }
    if (query.dueFrom) {
        conditions << QStringLiteral("due_at >= ?");
        bindings << Task::timestampToVariant(query.dueFrom);
    }
    if (query.dueBefore) {
        conditions << QStringLiteral("due_at < ?");
        bindings << Task::timestampToVariant(query.dueBefore);
    }

    if (!conditions.isEmpty())
        sql += QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" AND "));
    // Keep in step with Task::workflowLess(). Timestamps are fixed-width UTC
    // text, so comparing them as text is comparing them chronologically.
    sql += query.sort == SortOrder::Workflow
               ? QStringLiteral(" ORDER BY completed ASC, (due_at IS NULL) ASC, due_at ASC, "
                                "priority DESC, id ASC")
               : QStringLiteral(" ORDER BY id ASC");

    QSqlDatabase db = m_database.connection();
    QSqlQuery select(db);
    if (!select.prepare(sql))
        return fail(QStringLiteral("Cannot prepare task query: %1").arg(select.lastError().text()));
    for (const QVariant &binding : bindings)
        select.addBindValue(binding);
    if (!select.exec())
        return fail(QStringLiteral("Cannot load tasks: %1").arg(select.lastError().text()));

    // Title search is done here, not in SQL: SQLite's LIKE only folds ASCII
    // case, so "\u00FCber" would not find "\u00DCber". QString does full case
    // folding, and the needle is a plain substring (no wildcard characters).
    const QString needle = query.titleContains.trimmed();

    QList<Task> result;
    while (select.next()) {
        Task task = taskFromRow(select);
        if (!needle.isEmpty() && !task.title.contains(needle, Qt::CaseInsensitive))
            continue;
        result.append(std::move(task));
    }
    if (tasks)
        *tasks = result;
    return true;
}

bool TaskRepository::listNames(QStringList *names)
{
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    QSqlQuery select(db);
    if (!select.exec(QStringLiteral(
            "SELECT DISTINCT list_name FROM tasks ORDER BY list_name COLLATE NOCASE"))) {
        return fail(QStringLiteral("Cannot load lists: %1").arg(select.lastError().text()));
    }
    QStringList result;
    while (select.next())
        result << select.value(0).toString();
    if (names)
        *names = result;
    return true;
}

bool TaskRepository::getTask(qint64 id, Task *task)
{
    Query query;
    query.id = id;
    QList<Task> found;
    if (!loadTasks(query, &found))
        return false;
    if (found.isEmpty())
        return fail(QStringLiteral("Task %1 was not found.").arg(id));
    if (task)
        *task = found.first();
    return true;
}

bool TaskRepository::addTask(const Task &draft, Task *created)
{
    if (!ensureReady())
        return false;
    Task task = draft;
    if (!validate(&task))
        return false;

    // The row and (once an account is linked) its outbox operation commit together.
    DbTransaction tx(m_database.connection());
    if (!tx.begin())
        return fail(QStringLiteral("Cannot start transaction: %1").arg(tx.error()));

    qint64 newId = 0;
    {
        QSqlDatabase db = m_database.connection();
        QSqlQuery insert(db);
        if (!insert.prepare(QStringLiteral(
                "INSERT INTO tasks (title, notes, list_name, priority, due_at, completed, "
                "reminder_at, reminder_sent, created_at, updated_at, completed_at, uuid) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?)"))) {
            return fail(QStringLiteral("Cannot prepare insert: %1").arg(insert.lastError().text()));
        }
        const QVariant now = nowText();
        insert.addBindValue(task.title);
        insert.addBindValue(task.notes);
        insert.addBindValue(task.listName);
        insert.addBindValue(task.priority);
        insert.addBindValue(Task::timestampToVariant(task.dueAt));
        insert.addBindValue(task.completed ? 1 : 0);
        insert.addBindValue(Task::timestampToVariant(task.reminderAt));
        insert.addBindValue(now);
        insert.addBindValue(now);
        insert.addBindValue(task.completed ? now : QVariant(QMetaType::fromType<QString>()));
        insert.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!insert.exec())
            return fail(QStringLiteral("Cannot add task: %1").arg(insert.lastError().text()));
        newId = insert.lastInsertId().toLongLong();
    }
    Task stored;
    if (!getTask(newId, &stored))
        return false;
    if (!m_sync.recordCreate(syncJson(stored)))
        return fail(m_sync.lastError());
    if (!tx.commit())
        return fail(QStringLiteral("Cannot save the task: %1").arg(tx.error()));
    if (created)
        *created = stored;
    return true;
}

bool TaskRepository::updateTask(const Task &task, Task *updated)
{
    if (!ensureReady())
        return false;
    Task edited = task;
    if (!validate(&edited))
        return false;

    DbTransaction tx(m_database.connection());
    if (!tx.begin())
        return fail(QStringLiteral("Cannot start transaction: %1").arg(tx.error()));
    Task before;
    if (!getTask(edited.id, &before))
        return false;

    {
        QSqlDatabase db = m_database.connection();
        QSqlQuery update(db);
        // `reminder_at IS ?` compares against the value before this update.
        if (!update.prepare(QStringLiteral(
                "UPDATE tasks SET title = ?, notes = ?, list_name = ?, priority = ?, due_at = ?, "
                "reminder_sent = CASE WHEN reminder_at IS ? THEN reminder_sent ELSE 0 END, "
                "reminder_at = ?, updated_at = ? WHERE id = ?"))) {
            return fail(QStringLiteral("Cannot prepare update: %1").arg(update.lastError().text()));
        }
        const QVariant reminder = Task::timestampToVariant(edited.reminderAt);
        update.addBindValue(edited.title);
        update.addBindValue(edited.notes);
        update.addBindValue(edited.listName);
        update.addBindValue(edited.priority);
        update.addBindValue(Task::timestampToVariant(edited.dueAt));
        update.addBindValue(reminder);
        update.addBindValue(reminder);
        update.addBindValue(nowText());
        update.addBindValue(edited.id);
        if (!update.exec())
            return fail(QStringLiteral("Cannot update task: %1").arg(update.lastError().text()));
        if (update.numRowsAffected() == 0)
            return fail(QStringLiteral("Task %1 was not found.").arg(edited.id));
    }
    Task after;
    if (!getTask(edited.id, &after))
        return false;
    if (!m_sync.recordEdit(syncJson(before), syncJson(after)))
        return fail(m_sync.lastError());
    if (!tx.commit())
        return fail(QStringLiteral("Cannot save the task: %1").arg(tx.error()));
    if (updated)
        *updated = after;
    return true;
}

bool TaskRepository::setCompleted(qint64 id, bool completed, Task *updated)
{
    if (!ensureReady())
        return false;

    DbTransaction tx(m_database.connection());
    if (!tx.begin())
        return fail(QStringLiteral("Cannot start transaction: %1").arg(tx.error()));
    Task before;
    if (!getTask(id, &before))
        return false;

    {
        QSqlDatabase db = m_database.connection();
        QSqlQuery update(db);
        if (!update.prepare(QStringLiteral(
                "UPDATE tasks SET "
                // Re-completing keeps the original completion time; reopening clears it.
                "completed_at = CASE WHEN ? = 0 THEN NULL "
                "WHEN completed = 1 AND completed_at IS NOT NULL THEN completed_at ELSE ? END, "
                "completed = ?, updated_at = ? WHERE id = ?"))) {
            return fail(QStringLiteral("Cannot prepare update: %1").arg(update.lastError().text()));
        }
        const QVariant now = nowText();
        update.addBindValue(completed ? 1 : 0);
        update.addBindValue(now);
        update.addBindValue(completed ? 1 : 0);
        update.addBindValue(now);
        update.addBindValue(id);
        if (!update.exec())
            return fail(QStringLiteral("Cannot update task: %1").arg(update.lastError().text()));
        if (update.numRowsAffected() == 0)
            return fail(QStringLiteral("Task %1 was not found.").arg(id));
    }
    Task after;
    if (!getTask(id, &after))
        return false;
    if (!m_sync.recordEdit(syncJson(before), syncJson(after)))
        return fail(m_sync.lastError());
    if (!tx.commit())
        return fail(QStringLiteral("Cannot save the task: %1").arg(tx.error()));
    if (updated)
        *updated = after;
    return true;
}

namespace {

// " AND id NOT IN (?, ?, ...)" for a non-empty list, else an empty string.
QString excludeClause(const QList<qint64> &exclude)
{
    if (exclude.isEmpty())
        return QString();
    return QStringLiteral(" AND id NOT IN (%1)")
        .arg(QStringList(exclude.size(), QStringLiteral("?")).join(QStringLiteral(", ")));
}

} // namespace

bool TaskRepository::dueReminders(const QDateTime &now, const QList<qint64> &exclude,
                                  QList<Task> *tasks)
{
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    QSqlQuery select(db);
    const QString sql = QStringLiteral(
        "SELECT %1 FROM tasks WHERE reminder_at IS NOT NULL AND reminder_sent = 0 "
        "AND completed = 0 AND reminder_at <= ?%2 ORDER BY reminder_at ASC, id ASC")
        .arg(kColumns, excludeClause(exclude));
    if (!select.prepare(sql))
        return fail(QStringLiteral("Cannot prepare reminder query: %1").arg(select.lastError().text()));
    select.addBindValue(Task::timestampToVariant(now));
    for (qint64 id : exclude)
        select.addBindValue(id);
    if (!select.exec())
        return fail(QStringLiteral("Cannot load reminders: %1").arg(select.lastError().text()));

    QList<Task> result;
    while (select.next())
        result.append(taskFromRow(select));
    if (tasks)
        *tasks = result;
    return true;
}

bool TaskRepository::nextPendingReminder(const QList<qint64> &exclude, Task *task, bool *found)
{
    if (found)
        *found = false;
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    QSqlQuery select(db);
    const QString sql = QStringLiteral(
        "SELECT %1 FROM tasks WHERE reminder_at IS NOT NULL AND reminder_sent = 0 "
        "AND completed = 0%2 ORDER BY reminder_at ASC, id ASC LIMIT 1")
        .arg(kColumns, excludeClause(exclude));
    if (!select.prepare(sql))
        return fail(QStringLiteral("Cannot prepare reminder query: %1").arg(select.lastError().text()));
    for (qint64 id : exclude)
        select.addBindValue(id);
    if (!select.exec())
        return fail(QStringLiteral("Cannot load reminders: %1").arg(select.lastError().text()));

    if (select.next()) {
        if (task)
            *task = taskFromRow(select);
        if (found)
            *found = true;
    }
    return true;
}

bool TaskRepository::markReminderSent(qint64 id, bool *changed)
{
    if (changed)
        *changed = false;
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    QSqlQuery update(db);
    if (!update.prepare(QStringLiteral(
            "UPDATE tasks SET reminder_sent = 1 WHERE id = ? AND reminder_sent = 0")))
        return fail(QStringLiteral("Cannot prepare update: %1").arg(update.lastError().text()));
    update.addBindValue(id);
    if (!update.exec())
        return fail(QStringLiteral("Cannot record reminder: %1").arg(update.lastError().text()));
    if (changed)
        *changed = update.numRowsAffected() > 0;
    return true;
}

bool TaskRepository::restoreTask(const Task &snapshot, Task *restored)
{
    if (!ensureReady())
        return false;
    if (snapshot.id <= 0)
        return fail(QStringLiteral("That task cannot be restored (it has no id)."));
    Task task = snapshot;
    if (!validate(&task))
        return false;
    if (task.uuid.isEmpty())
        task.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

    DbTransaction tx(m_database.connection());
    if (!tx.begin())
        return fail(QStringLiteral("Cannot start transaction: %1").arg(tx.error()));

    {
        QSqlDatabase db = m_database.connection();
        QSqlQuery insert(db);
        if (!insert.prepare(QStringLiteral(
                "INSERT INTO tasks (id, title, notes, list_name, priority, due_at, completed, "
                "reminder_at, reminder_sent, created_at, updated_at, completed_at, uuid, server_rev) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                "(SELECT rev FROM sync_base WHERE task_uuid = ?))"))) {
            return fail(QStringLiteral("Cannot prepare restore: %1").arg(insert.lastError().text()));
        }
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QDateTime created = task.createdAt.isValid() ? task.createdAt : now;
        const QDateTime updated = task.updatedAt.isValid() ? task.updatedAt : now;
        insert.addBindValue(task.id);
        insert.addBindValue(task.title);
        insert.addBindValue(task.notes);
        insert.addBindValue(task.listName);
        insert.addBindValue(task.priority);
        insert.addBindValue(Task::timestampToVariant(task.dueAt));
        insert.addBindValue(task.completed ? 1 : 0);
        insert.addBindValue(Task::timestampToVariant(task.reminderAt));
        insert.addBindValue(task.reminderSent ? 1 : 0);
        insert.addBindValue(created.toUTC().toString(Qt::ISODateWithMs));
        insert.addBindValue(updated.toUTC().toString(Qt::ISODateWithMs));
        // A task restored as completed keeps its original completion time.
        std::optional<QDateTime> completedAt = task.completedAt;
        if (task.completed && !completedAt)
            completedAt = updated;
        insert.addBindValue(task.completed ? Task::timestampToVariant(completedAt)
                                           : QVariant(QMetaType::fromType<QString>()));
        insert.addBindValue(task.uuid);
        insert.addBindValue(task.uuid);
        if (!insert.exec()) {
            const QString reason = insert.lastError().text();
            if (reason.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive)
                || reason.contains(QStringLiteral("PRIMARY KEY"), Qt::CaseInsensitive)) {
                return fail(QStringLiteral("That task cannot be restored because its place has been taken."));
            }
            return fail(QStringLiteral("Cannot restore task: %1").arg(reason));
        }
    }
    Task back;
    if (!getTask(task.id, &back))
        return false;
    if (!m_sync.recordUndoDelete(syncJson(back)))
        return fail(m_sync.lastError());
    if (!tx.commit())
        return fail(QStringLiteral("Cannot restore the task: %1").arg(tx.error()));
    if (restored)
        *restored = back;
    return true;
}

bool TaskRepository::deleteTask(qint64 id, const QString &reason)
{
    if (!ensureReady())
        return false;

    DbTransaction tx(m_database.connection());
    if (!tx.begin())
        return fail(QStringLiteral("Cannot start transaction: %1").arg(tx.error()));
    Task task;
    if (!getTask(id, &task))
        return false;
    // Recorded while the row still exists: the operation needs its revision.
    if (!m_sync.recordDelete(syncJson(task), id, reason))
        return fail(m_sync.lastError());

    {
        QSqlDatabase db = m_database.connection();
        QSqlQuery remove(db);
        if (!remove.prepare(QStringLiteral("DELETE FROM tasks WHERE id = ?")))
            return fail(QStringLiteral("Cannot prepare delete: %1").arg(remove.lastError().text()));
        remove.addBindValue(id);
        if (!remove.exec())
            return fail(QStringLiteral("Cannot delete task: %1").arg(remove.lastError().text()));
        if (remove.numRowsAffected() == 0)
            return fail(QStringLiteral("Task %1 was not found.").arg(id));
    }
    if (!tx.commit())
        return fail(QStringLiteral("Cannot delete the task: %1").arg(tx.error()));
    return true;
}

// ---- completed-task cleanup ------------------------------------------------

bool TaskRepository::countCompleted(const std::optional<QDateTime> &completedBefore, int *count)
{
    if (count)
        *count = 0;
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    QSqlQuery select(db);
    const bool bounded = completedBefore.has_value();
    const QString sql = bounded
        ? QStringLiteral("SELECT COUNT(*) FROM tasks WHERE completed = 1 "
                         "AND completed_at IS NOT NULL AND completed_at <= ?")
        : QStringLiteral("SELECT COUNT(*) FROM tasks WHERE completed = 1");
    if (!select.prepare(sql))
        return fail(QStringLiteral("Cannot prepare count: %1").arg(select.lastError().text()));
    if (bounded)
        select.addBindValue(Task::timestampToVariant(completedBefore));
    if (!select.exec() || !select.next())
        return fail(QStringLiteral("Cannot count completed tasks: %1").arg(select.lastError().text()));
    if (count)
        *count = select.value(0).toInt();
    return true;
}

bool TaskRepository::purgeCompleted(const std::optional<QDateTime> &completedBefore,
                                    QList<qint64> *deletedIds)
{
    if (deletedIds)
        deletedIds->clear();
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    const bool bounded = completedBefore.has_value();
    // Only completed rows are ever matched, and a bounded purge also needs a
    // known completion time: a task of unknown age is kept.
    const QString where = bounded
        ? QStringLiteral("completed = 1 AND completed_at IS NOT NULL AND completed_at <= ?")
        : QStringLiteral("completed = 1");
    const QVariant cutoff = Task::timestampToVariant(completedBefore);

    DbTransaction tx(db);
    if (!tx.begin())
        return fail(QStringLiteral("Cannot start transaction: %1").arg(tx.error()));

    QList<qint64> ids;
    {
        QSqlQuery select(db);
        if (!select.prepare(QStringLiteral("SELECT id FROM tasks WHERE %1 ORDER BY id").arg(where)))
            return fail(QStringLiteral("Cannot prepare cleanup: %1").arg(select.lastError().text()));
        if (bounded)
            select.addBindValue(cutoff);
        if (!select.exec())
            return fail(QStringLiteral("Cannot find completed tasks: %1").arg(select.lastError().text()));
        while (select.next())
            ids << select.value(0).toLongLong();
    }

    // Every deletion syncs as a tombstone with its reason (SYNC 7). Recorded
    // before the rows go: each operation needs the task's revision.
    const QString reason = bounded ? QStringLiteral("retention") : QStringLiteral("clear_completed");
    for (qint64 id : ids) {
        Task task;
        if (!getTask(id, &task))
            return false;
        if (!m_sync.recordDelete(syncJson(task), id, reason))
            return fail(m_sync.lastError());
    }

    if (!ids.isEmpty()) {
        QSqlQuery remove(db);
        if (!remove.prepare(QStringLiteral("DELETE FROM tasks WHERE %1").arg(where)))
            return fail(QStringLiteral("Cannot prepare cleanup: %1").arg(remove.lastError().text()));
        if (bounded)
            remove.addBindValue(cutoff);
        if (!remove.exec())
            return fail(QStringLiteral("Cannot delete completed tasks: %1").arg(remove.lastError().text()));
        if (remove.numRowsAffected() != ids.size()) {
            return fail(QStringLiteral("Cleanup was cancelled because the set of completed "
                                       "tasks changed unexpectedly. Nothing was deleted."));
        }
    }

    if (!tx.commit())
        return fail(QStringLiteral("Cannot commit cleanup: %1").arg(tx.error()));
    if (deletedIds)
        *deletedIds = ids;
    return true;
}

bool TaskRepository::readMetadata(const QString &key, QString *value, bool *found)
{
    if (found)
        *found = false;
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    QSqlQuery select(db);
    if (!select.prepare(QStringLiteral("SELECT value FROM app_metadata WHERE key = ?")))
        return fail(QStringLiteral("Cannot prepare settings query: %1").arg(select.lastError().text()));
    select.addBindValue(key);
    if (!select.exec())
        return fail(QStringLiteral("Cannot read settings: %1").arg(select.lastError().text()));
    if (select.next()) {
        if (value)
            *value = select.value(0).toString();
        if (found)
            *found = true;
    }
    return true;
}

bool TaskRepository::writeMetadata(const QString &key, const QString &value)
{
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    QSqlQuery upsert(db);
    if (!upsert.prepare(QStringLiteral(
            "INSERT INTO app_metadata (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"))) {
        return fail(QStringLiteral("Cannot prepare settings update: %1").arg(upsert.lastError().text()));
    }
    upsert.addBindValue(key);
    upsert.addBindValue(value);
    if (!upsert.exec())
        return fail(QStringLiteral("Cannot save settings: %1").arg(upsert.lastError().text()));
    return true;
}
