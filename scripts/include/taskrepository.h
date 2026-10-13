#pragma once

#include "task.h"

#include <QList>
#include <QStringList>
#include <QString>

#include <optional>

class Database;

// SQL access for tasks, on the connection owned by Database. Every method
// returns false on failure and describes the reason in lastError(); a failed
// call never reports success and never substitutes empty results.
class TaskRepository
{
public:
    enum class CompletionFilter { All, Active, Completed };

    struct Query
    {
        std::optional<qint64> id;       // restrict to one task
        CompletionFilter completion = CompletionFilter::All;
        QString titleContains;          // literal substring, case-insensitive for ASCII
        QString listName;               // exact list name; empty = any list
        std::optional<QDateTime> dueFrom;   // due_at >= dueFrom (tasks without a due date never match)
        std::optional<QDateTime> dueBefore; // due_at <  dueBefore
    };

    // `database` must outlive the repository.
    explicit TaskRepository(Database &database);

    // Tasks matching `query`, oldest (lowest id) first.
    bool loadTasks(const Query &query, QList<Task> *tasks);
    bool getTask(qint64 id, Task *task);
    // Distinct list names in use, sorted case-insensitively.
    bool listNames(QStringList *names);

    // Title is trimmed and must be non-empty; priority must be 0..2.
    // `draft.id`, createdAt and updatedAt are ignored. Returns the stored task.
    bool addTask(const Task &draft, Task *created = nullptr);

    // Updates title, notes, list, priority, due date and reminder time.
    // Changing reminderAt re-arms the reminder (reminderSent = false).
    bool updateTask(const Task &task, Task *updated = nullptr);
    bool setCompleted(qint64 id, bool completed, Task *updated = nullptr);
    bool deleteTask(qint64 id);

    // ---- reminders -------------------------------------------------------
    // "Pending" = has a reminder time, not yet sent, task not completed.
    // `exclude` lists task ids to skip (reminders already being shown).

    // The pending reminder with the earliest time. `*found` tells whether one exists.
    bool nextPendingReminder(const QList<qint64> &exclude, Task *task, bool *found);
    // Pending reminders with reminder_at <= now, earliest first.
    bool dueReminders(const QDateTime &now, const QList<qint64> &exclude, QList<Task> *tasks);
    // Marks the reminder as sent; *changed is false if it was already sent or
    // the task no longer exists (so a reminder is never recorded twice).
    bool markReminderSent(qint64 id, bool *changed = nullptr);

    QString lastError() const { return m_lastError; }

private:
    bool ensureReady();
    bool fail(const QString &message);
    bool validate(Task *task);

    Database &m_database;
    QString m_lastError;
};
