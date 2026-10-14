#pragma once

#include "cleanuppolicy.h"
#include "task.h"
#include "taskrepository.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QList>
#include <QStringList>

// List model over TaskRepository. Rows are ordered by task id. Every mutating
// call changes the database first and the in-memory rows only on success, so
// the model always mirrors committed data (for the active view/filter/search).
// Lives on, and must be used from, its owning thread.
//
// The visible rows are defined by three independent settings that compose:
//   scope            which tasks (everything, due today, upcoming, one list)
//   completionFilter all / active / completed
//   searchText       title substring
class TaskListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int scope READ scope NOTIFY viewChanged)
    Q_PROPERTY(QString listName READ listName NOTIFY viewChanged)
    Q_PROPERTY(int completionFilter READ completionFilter WRITE setCompletionFilter NOTIFY completionFilterChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QStringList listNames READ listNames NOTIFY listNamesChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(QString undoTitle READ undoTitle NOTIFY undoChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorOccurred)
    // Completed-task cleanup policy: mode is a CleanupPolicy::Mode value
    // (0 never, 1 immediately, 2 after `cleanupDays` days).
    Q_PROPERTY(int cleanupMode READ cleanupMode NOTIFY cleanupPolicyChanged)
    Q_PROPERTY(int cleanupDays READ cleanupDays NOTIFY cleanupPolicyChanged)
    Q_PROPERTY(QString cleanupDescription READ cleanupDescription NOTIFY cleanupPolicyChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        NotesRole,
        ListNameRole,
        PriorityRole,
        DueAtRole,
        CompletedRole,
        ReminderAtRole,   // QDateTime (local time) or null
        ReminderSentRole,
    };

    enum CompletionFilter { All, Active, Completed };
    Q_ENUM(CompletionFilter)

    enum Scope {
        Everything, // every task
        Today,      // due on the current local calendar day (any completion state)
        Upcoming,   // incomplete tasks due after today
        List,       // tasks in the list named by listName
    };
    Q_ENUM(Scope)

    // `repository` must outlive the model.
    explicit TaskListModel(TaskRepository &repository, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int scope() const { return static_cast<int>(m_scope); }
    QString listName() const { return m_query.listName; }
    int completionFilter() const { return static_cast<int>(m_query.completion); }
    void setCompletionFilter(int filter);
    QString searchText() const { return m_query.titleContains; }
    void setSearchText(const QString &text);
    QStringList listNames() const { return m_listNames; }
    QString lastError() const { return m_lastError; }
    bool canUndo() const { return m_undo.has_value(); }
    QString undoTitle() const { return m_undo ? m_undo->title : QString(); }

    int cleanupMode() const { return static_cast<int>(m_cleanup.mode); }
    int cleanupDays() const { return m_cleanup.days; }
    QString cleanupDescription() const { return m_cleanup.describe(); }

    // Selects scope (+ list name for List) and completion filter in one load.
    Q_INVOKABLE bool setView(int scope, const QString &listName = QString(),
                             int completionFilter = All);

    // Reloads from the database. On failure the current rows are kept.
    Q_INVOKABLE bool refresh();
    Q_INVOKABLE bool addTask(const QString &title, const QString &notes = QString(),
                             const QString &listName = QString(), int priority = 0,
                             const QDateTime &dueAt = QDateTime(),
                             const QDateTime &reminderAt = QDateTime());
    Q_INVOKABLE bool updateTask(qint64 id, const QString &title, const QString &notes,
                                const QString &listName, int priority,
                                const QDateTime &dueAt = QDateTime());
    // Like updateTask, and also sets the reminder; an invalid `reminderAt`
    // clears it. (updateTask leaves the stored reminder untouched.)
    Q_INVOKABLE bool updateTaskWithReminder(qint64 id, const QString &title, const QString &notes,
                                            const QString &listName, int priority,
                                            const QDateTime &dueAt, const QDateTime &reminderAt);
    Q_INVOKABLE bool setCompleted(qint64 id, bool completed);
    Q_INVOKABLE bool removeTask(qint64 id);

    // Undo for the most recent delete only. The snapshot lives in memory, is
    // replaced by the next delete, and is gone when the app exits. A failed
    // undo keeps the snapshot so it can be retried.
    Q_INVOKABLE bool undoDelete();
    Q_INVOKABLE void discardUndo();

    // ---- completed-task cleanup ---------------------------------------------
    // Permanent: nothing removed here goes into the undo snapshot.
    //
    // Loads the saved policy. A missing, unreadable or invalid value means
    // "never automatically delete"; the stored value is not modified.
    Q_INVOKABLE bool loadCleanupPolicy();
    // Removes the completed tasks that the current policy says are due (none
    // for "never"). Call at startup and when the app is re-activated. Returns
    // the number removed, or -1 on failure (nothing removed).
    Q_INVOKABLE int applyCleanup();
    // How many completed tasks saving this policy would remove right away;
    // -1 if the mode/days are invalid or the database failed.
    Q_INVOKABLE int previewCleanup(int mode, int days);
    // Saves the policy, then applies it to existing completed tasks (the UI
    // asks for confirmation first when previewCleanup() > 0). Not saved on
    // failure. Returns false if saving or the immediate cleanup failed.
    Q_INVOKABLE bool setCleanupPolicy(int mode, int days);
    // Number of completed tasks in the database, or -1 on failure.
    Q_INVOKABLE int completedCount();
    // Permanently deletes every completed task whatever the policy. Returns the
    // number removed, or -1 on failure (nothing removed).
    Q_INVOKABLE int clearCompleted();

signals:
    void errorOccurred(const QString &message);
    void countChanged();
    void completionFilterChanged();
    void searchTextChanged();
    void viewChanged();
    void listNamesChanged();
    // Emitted after any successful add/update/complete/delete, whether or not
    // the change is visible in the current view.
    void tasksMutated();
    void undoChanged();
    void cleanupPolicyChanged();
    // The policy removed completed tasks without the user acting on them
    // individually (startup, re-activation, completion, policy change).
    void completedTasksRemoved(int count);

private:
    // `base` holds completion, search text and list name; the date range
    // implied by `scope` is computed from the current local date on each call.
    TaskRepository::Query effectiveQuery(const TaskRepository::Query &base, Scope scope) const;
    bool reload(const TaskRepository::Query &base, Scope scope);
    bool matchesFilter(const Task &task, bool *matches);
    bool applyChanged(const Task &task);
    void refreshListNames();
    int rowOfId(qint64 id) const;
    bool applyUpdate(qint64 id, const QString &title, const QString &notes,
                     const QString &listName, int priority, const QDateTime &dueAt,
                     const QDateTime *reminderAt);
    void removeRowsById(const QList<qint64> &ids);
    bool completeAndRemove(qint64 id);
    int purge(const std::optional<QDateTime> &cutoff, bool automatic);
    bool reportError(const QString &message);
    bool reportRepositoryError();

    TaskRepository &m_repository;
    TaskRepository::Query m_query;
    Scope m_scope = Everything;
    QList<Task> m_tasks;
    std::optional<Task> m_undo; // the last deleted task, until undone or discarded
    CleanupPolicy m_cleanup;
    QStringList m_listNames;
    QString m_lastError;
};
