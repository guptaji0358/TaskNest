#pragma once

#include "task.h"
#include "taskrepository.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QList>

// List model over TaskRepository. Rows are ordered by task id. Every mutating
// call changes the database first and the in-memory rows only on success, so
// the model always mirrors committed data (for the active filter/search).
// Lives on, and must be used from, its owning thread.
class TaskListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int completionFilter READ completionFilter WRITE setCompletionFilter NOTIFY completionFilterChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
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
    };

    enum CompletionFilter { All, Active, Completed };
    Q_ENUM(CompletionFilter)

    // `repository` must outlive the model.
    explicit TaskListModel(TaskRepository &repository, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int completionFilter() const { return static_cast<int>(m_query.completion); }
    void setCompletionFilter(int filter);
    QString searchText() const { return m_query.titleContains; }
    void setSearchText(const QString &text);
    QString lastError() const { return m_lastError; }

    // Reloads from the database. On failure the current rows are kept.
    Q_INVOKABLE bool refresh();
    Q_INVOKABLE bool addTask(const QString &title, const QString &notes = QString(),
                             const QString &listName = QString(), int priority = 0,
                             const QDateTime &dueAt = QDateTime());
    Q_INVOKABLE bool updateTask(qint64 id, const QString &title, const QString &notes,
                                const QString &listName, int priority,
                                const QDateTime &dueAt = QDateTime());
    Q_INVOKABLE bool setCompleted(qint64 id, bool completed);
    Q_INVOKABLE bool removeTask(qint64 id);

signals:
    void errorOccurred(const QString &message);
    void countChanged();
    void completionFilterChanged();
    void searchTextChanged();

private:
    bool reload(const TaskRepository::Query &query);
    bool matchesFilter(const Task &task, bool *matches);
    bool applyChanged(const Task &task);
    int rowOfId(qint64 id) const;
    bool reportError(const QString &message);
    bool reportRepositoryError();

    TaskRepository &m_repository;
    TaskRepository::Query m_query;
    QList<Task> m_tasks;
    QString m_lastError;
};
