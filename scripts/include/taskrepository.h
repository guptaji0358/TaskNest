#pragma once

#include "task.h"

#include <QList>
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
    };

    // `database` must outlive the repository.
    explicit TaskRepository(Database &database);

    // Tasks matching `query`, oldest (lowest id) first.
    bool loadTasks(const Query &query, QList<Task> *tasks);
    bool getTask(qint64 id, Task *task);

    // Title is trimmed and must be non-empty; priority must be 0..2.
    // `draft.id`, createdAt and updatedAt are ignored. Returns the stored task.
    bool addTask(const Task &draft, Task *created = nullptr);

    // Updates title, notes, list, priority, due date and reminder time.
    // Changing reminderAt re-arms the reminder (reminderSent = false).
    bool updateTask(const Task &task, Task *updated = nullptr);
    bool setCompleted(qint64 id, bool completed, Task *updated = nullptr);
    bool deleteTask(qint64 id);

    QString lastError() const { return m_lastError; }

private:
    bool ensureReady();
    bool fail(const QString &message);
    bool validate(Task *task);

    Database &m_database;
    QString m_lastError;
};
