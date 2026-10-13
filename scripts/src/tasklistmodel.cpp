#include "tasklistmodel.h"

#include <algorithm>

TaskListModel::TaskListModel(TaskRepository &repository, QObject *parent)
    : QAbstractListModel(parent)
    , m_repository(repository)
{
}

int TaskListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_tasks.size());
}

QVariant TaskListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tasks.size())
        return {};

    const Task &task = m_tasks.at(index.row());
    switch (role) {
    case IdRole:
        return task.id;
    case Qt::DisplayRole:
    case TitleRole:
        return task.title;
    case NotesRole:
        return task.notes;
    case ListNameRole:
        return task.listName;
    case PriorityRole:
        return task.priority;
    case DueAtRole:
        return task.dueAt ? QVariant(task.dueAt->toLocalTime()) : QVariant();
    case CompletedRole:
        return task.completed;
    default:
        return {};
    }
}

QHash<int, QByteArray> TaskListModel::roleNames() const
{
    return {
        {IdRole, "id"},
        {TitleRole, "title"},
        {NotesRole, "notes"},
        {ListNameRole, "listName"},
        {PriorityRole, "priority"},
        {DueAtRole, "dueAt"},
        {CompletedRole, "completed"},
    };
}

bool TaskListModel::reportError(const QString &message)
{
    m_lastError = message;
    emit errorOccurred(message);
    return false;
}

bool TaskListModel::reportRepositoryError()
{
    return reportError(m_repository.lastError());
}

int TaskListModel::rowOfId(qint64 id) const
{
    for (int row = 0; row < m_tasks.size(); ++row) {
        if (m_tasks.at(row).id == id)
            return row;
    }
    return -1;
}

bool TaskListModel::reload(const TaskRepository::Query &query)
{
    // Load first so a failure leaves the current rows untouched.
    QList<Task> loaded;
    if (!m_repository.loadTasks(query, &loaded))
        return reportRepositoryError();

    const int oldCount = static_cast<int>(m_tasks.size());
    beginResetModel();
    m_tasks = loaded;
    m_query = query;
    endResetModel();
    if (oldCount != m_tasks.size())
        emit countChanged();
    return true;
}

bool TaskListModel::refresh()
{
    return reload(m_query);
}

void TaskListModel::setCompletionFilter(int filter)
{
    if (filter < All || filter > Completed || filter == completionFilter())
        return;
    TaskRepository::Query query = m_query;
    query.completion = static_cast<TaskRepository::CompletionFilter>(filter);
    if (reload(query))
        emit completionFilterChanged();
}

void TaskListModel::setSearchText(const QString &text)
{
    if (text == m_query.titleContains)
        return;
    TaskRepository::Query query = m_query;
    query.titleContains = text;
    if (reload(query))
        emit searchTextChanged();
}

// Asks the database (so the answer matches SQL semantics exactly) whether the
// task belongs in the current view. Skipped when no filter is active.
bool TaskListModel::matchesFilter(const Task &task, bool *matches)
{
    const bool unfiltered = m_query.completion == TaskRepository::CompletionFilter::All
                            && m_query.titleContains.trimmed().isEmpty();
    if (unfiltered) {
        *matches = true;
        return true;
    }
    TaskRepository::Query query = m_query;
    query.id = task.id;
    QList<Task> found;
    if (!m_repository.loadTasks(query, &found))
        return false;
    *matches = !found.isEmpty();
    return true;
}

// Brings the rows in line with a task that was just stored or modified.
bool TaskListModel::applyChanged(const Task &task)
{
    bool matches = false;
    if (!matchesFilter(task, &matches)) {
        // The change is committed; only the view could not be decided.
        reportRepositoryError();
        refresh();
        return true;
    }

    const int row = rowOfId(task.id);
    if (row >= 0 && matches) {
        m_tasks[row] = task;
        const QModelIndex index = this->index(row);
        emit dataChanged(index, index);
    } else if (row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        m_tasks.removeAt(row);
        endRemoveRows();
        emit countChanged();
    } else if (matches) {
        // Keep id order: new tasks append, edits to hidden tasks slot in.
        const auto position = std::lower_bound(
            m_tasks.begin(), m_tasks.end(), task.id,
            [](const Task &existing, qint64 id) { return existing.id < id; });
        const int insertRow = static_cast<int>(position - m_tasks.begin());
        beginInsertRows(QModelIndex(), insertRow, insertRow);
        m_tasks.insert(insertRow, task);
        endInsertRows();
        emit countChanged();
    }
    return true;
}

bool TaskListModel::addTask(const QString &title, const QString &notes, const QString &listName,
                            int priority, const QDateTime &dueAt)
{
    Task draft;
    draft.title = title;
    draft.notes = notes;
    if (!listName.isEmpty())
        draft.listName = listName;
    draft.priority = priority;
    if (dueAt.isValid())
        draft.dueAt = dueAt;

    Task created;
    if (!m_repository.addTask(draft, &created))
        return reportRepositoryError();
    return applyChanged(created);
}

bool TaskListModel::updateTask(qint64 id, const QString &title, const QString &notes,
                               const QString &listName, int priority, const QDateTime &dueAt)
{
    // Start from the stored task so fields not exposed here (reminder) survive.
    Task task;
    if (!m_repository.getTask(id, &task))
        return reportRepositoryError();
    task.title = title;
    task.notes = notes;
    task.listName = listName;
    task.priority = priority;
    task.dueAt = dueAt.isValid() ? std::optional<QDateTime>(dueAt) : std::nullopt;

    Task updated;
    if (!m_repository.updateTask(task, &updated))
        return reportRepositoryError();
    return applyChanged(updated);
}

bool TaskListModel::setCompleted(qint64 id, bool completed)
{
    Task updated;
    if (!m_repository.setCompleted(id, completed, &updated))
        return reportRepositoryError();
    return applyChanged(updated);
}

bool TaskListModel::removeTask(qint64 id)
{
    if (!m_repository.deleteTask(id))
        return reportRepositoryError();

    const int row = rowOfId(id);
    if (row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        m_tasks.removeAt(row);
        endRemoveRows();
        emit countChanged();
    }
    return true;
}
