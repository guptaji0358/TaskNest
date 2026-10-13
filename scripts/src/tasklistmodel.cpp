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
    case ReminderAtRole:
        return task.reminderAt ? QVariant(task.reminderAt->toLocalTime()) : QVariant();
    case ReminderSentRole:
        return task.reminderSent;
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
        {ReminderAtRole, "reminderAt"},
        {ReminderSentRole, "reminderSent"},
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

TaskRepository::Query TaskListModel::effectiveQuery(const TaskRepository::Query &base,
                                                    Scope scope) const
{
    TaskRepository::Query query = base;
    query.sort = TaskRepository::SortOrder::Workflow;
    if (scope != List)
        query.listName.clear();

    // Day boundaries are local midnights; the repository compares them as UTC.
    const QDate today = QDate::currentDate();
    switch (scope) {
    case Today:
        query.dueFrom = QDateTime(today, QTime(0, 0));
        query.dueBefore = QDateTime(today.addDays(1), QTime(0, 0));
        break;
    case Upcoming:
        query.dueFrom = QDateTime(today.addDays(1), QTime(0, 0));
        query.completion = TaskRepository::CompletionFilter::Active;
        break;
    case Everything:
    case List:
        break;
    }
    return query;
}

bool TaskListModel::reload(const TaskRepository::Query &base, Scope scope)
{
    // Load first so a failure leaves the current rows untouched.
    QList<Task> loaded;
    if (!m_repository.loadTasks(effectiveQuery(base, scope), &loaded))
        return reportRepositoryError();

    const int oldCount = static_cast<int>(m_tasks.size());
    beginResetModel();
    m_tasks = loaded;
    m_query = base;
    m_scope = scope;
    endResetModel();
    if (oldCount != m_tasks.size())
        emit countChanged();
    refreshListNames();
    return true;
}

bool TaskListModel::refresh()
{
    return reload(m_query, m_scope);
}

bool TaskListModel::setView(int scope, const QString &listName, int completionFilter)
{
    if (scope < Everything || scope > List || completionFilter < All || completionFilter > Completed)
        return reportError(QStringLiteral("Invalid view selection."));
    if (scope == List && listName.trimmed().isEmpty())
        return reportError(QStringLiteral("A list name is required."));

    TaskRepository::Query base = m_query;
    base.completion = static_cast<TaskRepository::CompletionFilter>(completionFilter);
    base.listName = scope == List ? listName.trimmed() : QString();

    const bool completionChanged = base.completion != m_query.completion;
    if (!reload(base, static_cast<Scope>(scope)))
        return false;
    emit viewChanged();
    if (completionChanged)
        emit completionFilterChanged();
    return true;
}

void TaskListModel::setCompletionFilter(int filter)
{
    if (filter < All || filter > Completed || filter == completionFilter())
        return;
    TaskRepository::Query base = m_query;
    base.completion = static_cast<TaskRepository::CompletionFilter>(filter);
    if (reload(base, m_scope))
        emit completionFilterChanged();
}

void TaskListModel::setSearchText(const QString &text)
{
    if (text == m_query.titleContains)
        return;
    TaskRepository::Query base = m_query;
    base.titleContains = text;
    if (reload(base, m_scope))
        emit searchTextChanged();
}

void TaskListModel::refreshListNames()
{
    QStringList names;
    if (!m_repository.listNames(&names)) {
        reportRepositoryError();
        return;
    }
    if (names != m_listNames) {
        m_listNames = names;
        emit listNamesChanged();
    }
}

// Asks the database (so the answer matches SQL semantics exactly) whether the
// task belongs in the current view. Skipped when nothing narrows the view.
bool TaskListModel::matchesFilter(const Task &task, bool *matches)
{
    const bool unfiltered = m_scope == Everything
                            && m_query.completion == TaskRepository::CompletionFilter::All
                            && m_query.titleContains.trimmed().isEmpty();
    if (unfiltered) {
        *matches = true;
        return true;
    }
    TaskRepository::Query query = effectiveQuery(m_query, m_scope);
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
    refreshListNames();
    emit tasksMutated();

    bool matches = false;
    if (!matchesFilter(task, &matches)) {
        // The change is committed; only the view could not be decided.
        reportRepositoryError();
        refresh();
        return true;
    }

    const int row = rowOfId(task.id);

    // Where the task belongs among the other rows, in workflow order.
    auto positionAmongOthers = [this, &task, row]() {
        int position = 0;
        for (int i = 0; i < m_tasks.size(); ++i) {
            if (i != row && Task::workflowLess(m_tasks.at(i), task))
                ++position;
        }
        return position;
    };

    if (row >= 0 && matches) {
        const int target = positionAmongOthers();
        if (target == row) {
            m_tasks[row] = task;
            const QModelIndex index = this->index(row);
            emit dataChanged(index, index);
        } else {
            // Completion, due date or priority changed: the row moves.
            const int destination = target > row ? target + 1 : target;
            beginMoveRows(QModelIndex(), row, row, QModelIndex(), destination);
            m_tasks.move(row, target);
            endMoveRows();
            m_tasks[target] = task;
            const QModelIndex index = this->index(target);
            emit dataChanged(index, index);
        }
    } else if (row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        m_tasks.removeAt(row);
        endRemoveRows();
        emit countChanged();
    } else if (matches) {
        const int insertRow = positionAmongOthers();
        beginInsertRows(QModelIndex(), insertRow, insertRow);
        m_tasks.insert(insertRow, task);
        endInsertRows();
        emit countChanged();
    }
    return true;
}

bool TaskListModel::addTask(const QString &title, const QString &notes, const QString &listName,
                            int priority, const QDateTime &dueAt, const QDateTime &reminderAt)
{
    Task draft;
    draft.title = title;
    draft.notes = notes;
    if (!listName.isEmpty())
        draft.listName = listName;
    draft.priority = priority;
    if (dueAt.isValid())
        draft.dueAt = dueAt;
    if (reminderAt.isValid())
        draft.reminderAt = reminderAt;

    Task created;
    if (!m_repository.addTask(draft, &created))
        return reportRepositoryError();
    return applyChanged(created);
}

bool TaskListModel::updateTask(qint64 id, const QString &title, const QString &notes,
                               const QString &listName, int priority, const QDateTime &dueAt)
{
    return applyUpdate(id, title, notes, listName, priority, dueAt, nullptr);
}

bool TaskListModel::updateTaskWithReminder(qint64 id, const QString &title, const QString &notes,
                                           const QString &listName, int priority,
                                           const QDateTime &dueAt, const QDateTime &reminderAt)
{
    return applyUpdate(id, title, notes, listName, priority, dueAt, &reminderAt);
}

bool TaskListModel::applyUpdate(qint64 id, const QString &title, const QString &notes,
                                const QString &listName, int priority, const QDateTime &dueAt,
                                const QDateTime *reminderAt)
{
    // Start from the stored task so fields not passed here survive.
    Task task;
    if (!m_repository.getTask(id, &task))
        return reportRepositoryError();
    task.title = title;
    task.notes = notes;
    task.listName = listName;
    task.priority = priority;
    task.dueAt = dueAt.isValid() ? std::optional<QDateTime>(dueAt) : std::nullopt;
    if (reminderAt) {
        task.reminderAt = reminderAt->isValid() ? std::optional<QDateTime>(*reminderAt)
                                                : std::nullopt;
    }

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
    // Take the snapshot first: it is what "Undo" puts back.
    Task snapshot;
    const int snapshotRow = rowOfId(id);
    const bool haveSnapshot = snapshotRow >= 0
        ? (snapshot = m_tasks.at(snapshotRow), true)
        : m_repository.getTask(id, &snapshot);

    if (!m_repository.deleteTask(id))
        return reportRepositoryError();

    if (haveSnapshot) {
        m_undo = snapshot;
        emit undoChanged();
    }

    const int row = rowOfId(id);
    if (row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        m_tasks.removeAt(row);
        endRemoveRows();
        emit countChanged();
    }
    refreshListNames();
    emit tasksMutated();
    return true;
}

bool TaskListModel::undoDelete()
{
    if (!m_undo)
        return reportError(QStringLiteral("There is nothing to undo."));

    Task restored;
    if (!m_repository.restoreTask(*m_undo, &restored))
        return reportRepositoryError(); // snapshot kept: the caller may retry

    // Consume the snapshot before touching the view so a second Undo cannot
    // restore twice.
    m_undo.reset();
    emit undoChanged();
    return applyChanged(restored);
}

void TaskListModel::discardUndo()
{
    if (!m_undo)
        return;
    m_undo.reset();
    emit undoChanged();
}
