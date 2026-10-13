#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

class QAbstractNativeEventFilter;
class TaskRepository;

// Fires task reminders while the application is running.
//
// Design
//  * One single-shot QTimer, armed for the earliest pending reminder. There is
//    no polling: when nothing is pending the timer is not running.
//  * When the timer fires the database is queried again (the clock and the
//    data may have changed), every due reminder is announced once through
//    reminderFired(), and the next reminder is scheduled.
//  * A reminder counts as "sent" only when the user acknowledges it
//    (acknowledge()). Until then it is "in flight": it is not announced again
//    and it is not scheduled again, but it is announced again after a restart
//    because the database still says "not sent". Nothing is lost if the app
//    closes before the user has seen it.
//  * Catch-up policy: reminders that are already overdue at startup, on
//    resume from sleep, or when the window is re-activated are announced
//    immediately, oldest first, each exactly once, however old they are.
//  * reschedule() must be called after anything that can change reminders
//    (task edits, completion, deletion), when the application becomes active,
//    and after resume from sleep (watchSystemResume() wires the latter).
//
// Reminders are only delivered while the application process is running.
// Nothing is registered with Windows (no service, task or startup entry).
class ReminderScheduler : public QObject
{
    Q_OBJECT

public:
    using Clock = std::function<QDateTime()>;

    explicit ReminderScheduler(TaskRepository &repository, QObject *parent = nullptr);
    ~ReminderScheduler() override;

    // Replaces the time source (tests). Defaults to QDateTime::currentDateTimeUtc().
    void setClock(Clock clock);

    // Re-evaluates the database and (re)arms the timer. Safe to call often.
    Q_INVOKABLE void reschedule();
    // Announces every reminder that is due now, then schedules the next one.
    // Called by the timer; public so tests can drive it without waiting.
    void processDue();
    // Records that the user has handled the reminder. Returns false (and
    // keeps the reminder pending) if it could not be stored.
    Q_INVOKABLE bool acknowledge(qint64 taskId);

    void stop();
    // Windows only: reschedules when the system resumes from sleep.
    void watchSystemResume();

    // Introspection (tests, diagnostics).
    bool isTimerActive() const { return m_timer.isActive(); }
    int timerInterval() const { return m_timer.interval(); }
    QDateTime nextReminderAt() const { return m_nextAt; }
    QList<qint64> inFlightIds() const { return m_inFlight.keys(); }

signals:
    // `reminderAt` is in local time.
    void reminderFired(qint64 taskId, const QString &title, const QDateTime &reminderAt);
    // A reminder that was announced is no longer relevant (task completed,
    // deleted, or its reminder time changed). The UI should drop it.
    void reminderCancelled(qint64 taskId);
    void errorOccurred(const QString &message);

private:
    QDateTime now() const;
    void validateInFlight();
    void armRetry(const QString &message);

    TaskRepository &m_repository;
    Clock m_clock;
    QTimer m_timer;
    QDateTime m_nextAt;
    QHash<qint64, QDateTime> m_inFlight; // task id -> reminder time that was announced
    std::unique_ptr<QAbstractNativeEventFilter> m_resumeFilter;
};
