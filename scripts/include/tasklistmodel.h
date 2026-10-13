#pragma once

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
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorOccurred)

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

    // Selects scope (+ list name for List) and completion filter in one load.
    Q_INVOKABLE bool setView(int scope, const QString &listName = QString(),
                             int completionFilter = All);

    // Reloads from the database. On failure the current rows are kept.
    Q_INVOKABLE bool refresh();
    Q_INVOKABLE bool addTask(const QString &title, const QString &notes = QString(),
                             const QString &listName = QString(), int priority = 0,
                             const QDateTime &dueAt = QDateTime());
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
    bool reportError(const QString &message);
    bool reportRepositoryError();

    TaskRepository &m_repository;
    TaskRepository::Query m_query;
    Scope m_scope = Everything;
    QList<Task> m_tasks;
    QStringList m_listNames;
    QString m_lastError;
};
