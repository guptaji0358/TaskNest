#include "taskrepository.h"

#include "database.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariantList>

namespace {

const QString kColumns = QStringLiteral(
    "id, title, notes, list_name, priority, due_at, completed, reminder_at, reminder_sent, "
    "created_at, updated_at");

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
    return task;
}

// Makes `text` match literally inside a LIKE pattern that uses ESCAPE '\'.
QString escapeLike(QString text)
{
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    text.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return text;
}

QVariant nowText()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

} // namespace

TaskRepository::TaskRepository(Database &database)
    : m_database(database)
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

    const QString needle = query.titleContains.trimmed();
    if (!needle.isEmpty()) {
        conditions << QStringLiteral("title LIKE ? ESCAPE '\\'");
        bindings << QStringLiteral("%") + escapeLike(needle) + QStringLiteral("%");
    }

    if (!conditions.isEmpty())
        sql += QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY id ASC");

    QSqlDatabase db = m_database.connection();
    QSqlQuery select(db);
    if (!select.prepare(sql))
        return fail(QStringLiteral("Cannot prepare task query: %1").arg(select.lastError().text()));
    for (const QVariant &binding : bindings)
        select.addBindValue(binding);
    if (!select.exec())
        return fail(QStringLiteral("Cannot load tasks: %1").arg(select.lastError().text()));

    QList<Task> result;
    while (select.next())
        result.append(taskFromRow(select));
    if (tasks)
        *tasks = result;
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

    qint64 newId = 0;
    {
        QSqlDatabase db = m_database.connection();
        QSqlQuery insert(db);
        if (!insert.prepare(QStringLiteral(
                "INSERT INTO tasks (title, notes, list_name, priority, due_at, completed, "
                "reminder_at, reminder_sent, created_at, updated_at) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?)"))) {
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
        if (!insert.exec())
            return fail(QStringLiteral("Cannot add task: %1").arg(insert.lastError().text()));
        newId = insert.lastInsertId().toLongLong();
    }
    return getTask(newId, created);
}

bool TaskRepository::updateTask(const Task &task, Task *updated)
{
    if (!ensureReady())
        return false;
    Task edited = task;
    if (!validate(&edited))
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
    return getTask(edited.id, updated);
}

bool TaskRepository::setCompleted(qint64 id, bool completed, Task *updated)
{
    if (!ensureReady())
        return false;

    {
        QSqlDatabase db = m_database.connection();
        QSqlQuery update(db);
        if (!update.prepare(QStringLiteral(
                "UPDATE tasks SET completed = ?, updated_at = ? WHERE id = ?"))) {
            return fail(QStringLiteral("Cannot prepare update: %1").arg(update.lastError().text()));
        }
        update.addBindValue(completed ? 1 : 0);
        update.addBindValue(nowText());
        update.addBindValue(id);
        if (!update.exec())
            return fail(QStringLiteral("Cannot update task: %1").arg(update.lastError().text()));
        if (update.numRowsAffected() == 0)
            return fail(QStringLiteral("Task %1 was not found.").arg(id));
    }
    return getTask(id, updated);
}

bool TaskRepository::deleteTask(qint64 id)
{
    if (!ensureReady())
        return false;

    QSqlDatabase db = m_database.connection();
    QSqlQuery remove(db);
    if (!remove.prepare(QStringLiteral("DELETE FROM tasks WHERE id = ?")))
        return fail(QStringLiteral("Cannot prepare delete: %1").arg(remove.lastError().text()));
    remove.addBindValue(id);
    if (!remove.exec())
        return fail(QStringLiteral("Cannot delete task: %1").arg(remove.lastError().text()));
    if (remove.numRowsAffected() == 0)
        return fail(QStringLiteral("Task %1 was not found.").arg(id));
    return true;
}
