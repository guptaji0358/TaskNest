#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVariant>

#include <optional>

// A single task. Plain data; persistence lives in TaskRepository.
struct Task
{
    static constexpr int kMinPriority = 0;
    static constexpr int kMaxPriority = 2;

    qint64 id = 0; // 0 = not yet stored
    QString title;
    QString notes;
    QString listName = QStringLiteral("Inbox");
    int priority = 0;
    std::optional<QDateTime> dueAt;
    bool completed = false;
    std::optional<QDateTime> completedAt; // set while completed; basis for time-based cleanup
    std::optional<QDateTime> reminderAt;
    bool reminderSent = false;
    QDateTime createdAt;
    QDateTime updatedAt;

    // Display order: open tasks first; then earliest due date (undated last);
    // then higher priority; then creation order. TaskRepository's
    // SortOrder::Workflow SQL must stay equivalent to this.
    static bool workflowLess(const Task &a, const Task &b)
    {
        if (a.completed != b.completed)
            return !a.completed;
        if (a.dueAt.has_value() != b.dueAt.has_value())
            return a.dueAt.has_value();
        if (a.dueAt && *a.dueAt != *b.dueAt)
            return *a.dueAt < *b.dueAt;
        if (a.priority != b.priority)
            return a.priority > b.priority;
        return a.id < b.id;
    }

    static bool isValidPriority(int value) { return value >= kMinPriority && value <= kMaxPriority; }

    // Storage format for timestamps: ISO 8601 in UTC with milliseconds, which
    // also sorts chronologically as text. "No value" maps to a null QVariant.
    static QVariant timestampToVariant(const std::optional<QDateTime> &timestamp)
    {
        if (!timestamp || !timestamp->isValid())
            return QVariant(QMetaType::fromType<QString>());
        return timestamp->toUTC().toString(Qt::ISODateWithMs);
    }

    // Null or unparseable values yield std::nullopt.
    static std::optional<QDateTime> timestampFromVariant(const QVariant &value)
    {
        if (value.isNull())
            return std::nullopt;
        const QDateTime parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
        if (!parsed.isValid())
            return std::nullopt;
        return parsed.toUTC();
    }
};
