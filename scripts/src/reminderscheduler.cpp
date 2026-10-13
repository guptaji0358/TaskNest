#include "reminderscheduler.h"

#include "taskrepository.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// How long to wait before trying again after a database error. Only used
// while the database is failing; it is not a polling interval.
constexpr int kErrorRetryMs = 60 * 1000;

#ifdef Q_OS_WIN
// WM_POWERBROADCAST is delivered to top-level windows; the wall clock may have
// advanced a long way while the monotonic timer was suspended.
class ResumeFilter : public QAbstractNativeEventFilter
{
public:
    explicit ResumeFilter(std::function<void()> onResume)
        : m_onResume(std::move(onResume))
    {
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) override
    {
        if (eventType != "windows_generic_MSG")
            return false;
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message == WM_POWERBROADCAST
            && (msg->wParam == PBT_APMRESUMEAUTOMATIC || msg->wParam == PBT_APMRESUMESUSPEND)) {
            m_onResume();
        }
        return false; // never consume the event
    }

private:
    std::function<void()> m_onResume;
};
#endif

} // namespace

ReminderScheduler::ReminderScheduler(TaskRepository &repository, QObject *parent)
    : QObject(parent)
    , m_repository(repository)
    , m_clock([] { return QDateTime::currentDateTimeUtc(); })
{
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ReminderScheduler::processDue);
}

ReminderScheduler::~ReminderScheduler()
{
    stop();
}

void ReminderScheduler::setClock(Clock clock)
{
    m_clock = std::move(clock);
}

QDateTime ReminderScheduler::now() const
{
    return m_clock().toUTC();
}

void ReminderScheduler::stop()
{
    m_timer.stop();
    m_nextAt = QDateTime();
    m_inFlight.clear();
    if (m_resumeFilter) {
        if (QCoreApplication::instance())
            QCoreApplication::instance()->removeNativeEventFilter(m_resumeFilter.get());
        m_resumeFilter.reset();
    }
}

void ReminderScheduler::watchSystemResume()
{
#ifdef Q_OS_WIN
    if (m_resumeFilter || !QCoreApplication::instance())
        return;
    m_resumeFilter = std::make_unique<ResumeFilter>([this] { reschedule(); });
    QCoreApplication::instance()->installNativeEventFilter(m_resumeFilter.get());
#endif
}

void ReminderScheduler::armRetry(const QString &message)
{
    emit errorOccurred(message);
    m_nextAt = QDateTime();
    m_timer.start(kErrorRetryMs);
}

// Drops announced reminders that no longer apply.
void ReminderScheduler::validateInFlight()
{
    const QList<qint64> ids = m_inFlight.keys();
    for (qint64 id : ids) {
        Task task;
        bool cancel = false;
        bool completed = false;
        if (!m_repository.getTask(id, &task)) {
            // Distinguish "gone" from a database failure: only a missing task
            // may be cancelled; on failure keep it and let the caller retry.
            if (m_repository.lastError().contains(QStringLiteral("not found")))
                cancel = true;
            else
                continue;
        } else if (task.completed) {
            cancel = true;
            completed = true;
        } else if (task.reminderSent || !task.reminderAt
                   || task.reminderAt->toUTC() != m_inFlight.value(id)) {
            cancel = true;
        }

        if (cancel) {
            // The user dealt with the task itself: do not re-announce the
            // reminder if the task is re-opened later.
            if (completed)
                m_repository.markReminderSent(id);
            m_inFlight.remove(id);
            emit reminderCancelled(id);
        }
    }
}

void ReminderScheduler::reschedule()
{
    m_timer.stop();
    m_nextAt = QDateTime();

    validateInFlight();

    Task next;
    bool found = false;
    if (!m_repository.nextPendingReminder(m_inFlight.keys(), &next, &found)) {
        armRetry(m_repository.lastError());
        return;
    }
    if (!found)
        return; // nothing pending: no timer, nothing runs

    m_nextAt = next.reminderAt->toUTC();
    const qint64 waitMs = std::max<qint64>(0, now().msecsTo(m_nextAt));
    const qint64 maxMs = std::numeric_limits<int>::max();
    m_timer.start(static_cast<int>(std::min(waitMs, maxMs)));
}

void ReminderScheduler::processDue()
{
    m_timer.stop();

    QList<Task> due;
    if (!m_repository.dueReminders(now(), m_inFlight.keys(), &due)) {
        armRetry(m_repository.lastError());
        return;
    }

    for (const Task &task : due) {
        if (m_inFlight.contains(task.id))
            continue;
        m_inFlight.insert(task.id, task.reminderAt->toUTC());
        emit reminderFired(task.id, task.title, task.reminderAt->toLocalTime());
    }

    reschedule();
}

bool ReminderScheduler::acknowledge(qint64 taskId)
{
    if (!m_inFlight.contains(taskId))
        return true; // already handled or cancelled

    if (!m_repository.markReminderSent(taskId)) {
        emit errorOccurred(m_repository.lastError());
        return false; // stays pending; nothing was recorded
    }
    m_inFlight.remove(taskId);
    reschedule();
    return true;
}
