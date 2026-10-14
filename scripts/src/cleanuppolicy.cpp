#include "cleanuppolicy.h"

const QString &CleanupPolicy::metadataKey()
{
    static const QString key = QStringLiteral("completed_cleanup_policy");
    return key;
}

std::optional<CleanupPolicy> CleanupPolicy::fromParts(int mode, int days)
{
    switch (mode) {
    case static_cast<int>(Mode::Never):
        return never();
    case static_cast<int>(Mode::Immediately):
        return immediately();
    case static_cast<int>(Mode::AfterDays):
        if (days < kMinDays || days > kMaxDays)
            return std::nullopt;
        return afterDays(days);
    default:
        return std::nullopt;
    }
}

std::optional<CleanupPolicy> CleanupPolicy::fromStored(const QString &text)
{
    const QString value = text.trimmed();
    if (value == QStringLiteral("never"))
        return never();
    if (value == QStringLiteral("immediately"))
        return immediately();
    const QString prefix = QStringLiteral("days:");
    if (value.startsWith(prefix)) {
        bool ok = false;
        const int days = value.mid(prefix.size()).toInt(&ok);
        if (ok)
            return fromParts(static_cast<int>(Mode::AfterDays), days);
    }
    return std::nullopt;
}

QString CleanupPolicy::toStored() const
{
    switch (mode) {
    case Mode::Immediately:
        return QStringLiteral("immediately");
    case Mode::AfterDays:
        return QStringLiteral("days:%1").arg(days);
    case Mode::Never:
        break;
    }
    return QStringLiteral("never");
}

QString CleanupPolicy::describe() const
{
    switch (mode) {
    case Mode::Immediately:
        return QStringLiteral("Delete immediately when completed");
    case Mode::AfterDays:
        return days == 1 ? QStringLiteral("Delete 1 day after completion")
                         : QStringLiteral("Delete %1 days after completion").arg(days);
    case Mode::Never:
        break;
    }
    return QStringLiteral("Never automatically delete");
}

std::optional<QDateTime> CleanupPolicy::cutoff(const QDateTime &now) const
{
    switch (mode) {
    case Mode::Immediately:
        return now.toUTC();
    case Mode::AfterDays:
        return now.toUTC().addSecs(-static_cast<qint64>(days) * 24 * 60 * 60);
    case Mode::Never:
        break;
    }
    return std::nullopt;
}
