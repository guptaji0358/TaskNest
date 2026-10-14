#pragma once

#include <QDateTime>
#include <QString>

#include <optional>

// What happens to completed tasks. Stored in app_metadata under kMetadataKey
// as "never", "immediately" or "days:<N>". The default, and the fallback for a
// missing or unreadable value, is Never: nothing is ever deleted unless the
// user explicitly chose a policy.
struct CleanupPolicy
{
    enum class Mode { Never = 0, Immediately = 1, AfterDays = 2 };

    static constexpr int kMinDays = 1;
    static constexpr int kMaxDays = 3650;

    static const QString &metadataKey();

    Mode mode = Mode::Never;
    int days = 0; // only meaningful for AfterDays

    static CleanupPolicy never() { return {}; }
    static CleanupPolicy immediately() { return {Mode::Immediately, 0}; }
    static CleanupPolicy afterDays(int days) { return {Mode::AfterDays, days}; }

    // Validates a mode/days pair coming from the UI. nullopt if it is not a
    // policy the application offers.
    static std::optional<CleanupPolicy> fromParts(int mode, int days);
    // Parses the stored text; nullopt for anything that is not a valid policy.
    static std::optional<CleanupPolicy> fromStored(const QString &text);

    QString toStored() const;
    // Human-readable, for messages.
    QString describe() const;

    bool deletesAutomatically() const { return mode != Mode::Never; }

    // Completed tasks whose completed_at is at or before the returned instant
    // are due for removal. Rule: a task is removed once `days` x 24 hours have
    // fully elapsed since it was completed. Immediately -> `now` (every task
    // with a completion time); Never -> nullopt (nothing is due).
    std::optional<QDateTime> cutoff(const QDateTime &now) const;

    bool operator==(const CleanupPolicy &other) const
    {
        return mode == other.mode && days == other.days;
    }
    bool operator!=(const CleanupPolicy &other) const { return !(*this == other); }
};
