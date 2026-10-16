#include "singleinstance.h"

#include <QByteArray>
#include <QLockFile>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
struct FindWindowData
{
    DWORD pid = 0;
    HWND window = nullptr;
};

BOOL CALLBACK findWindowOfProcess(HWND window, LPARAM param)
{
    auto *data = reinterpret_cast<FindWindowData *>(param);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(window, &windowPid);
    // A visible top-level window (not an owned popup/tooltip) of that process.
    // The floating widget is a tool window; the main window is what to bring forward.
    const bool toolWindow = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    if (windowPid == data->pid && IsWindowVisible(window) && !toolWindow
        && GetWindow(window, GW_OWNER) == nullptr) {
        data->window = window;
        return FALSE; // stop
    }
    return TRUE;
}
#endif

} // namespace

SingleInstanceGuard::SingleInstanceGuard() = default;

SingleInstanceGuard::~SingleInstanceGuard()
{
    release();
}

bool SingleInstanceGuard::acquire(const QString &lockFilePath)
{
    release();
    m_ownerPid = 0;
    m_lockError.clear();

    auto lock = std::make_unique<QLockFile>(lockFilePath);
    // A lock older than this whose owner is not running is stale and is
    // removed by tryLock(); a running owner is never considered stale.
    lock->setStaleLockTime(15000);

    if (lock->tryLock(0)) {
        m_lock = std::move(lock);
        m_primary = true;
        return true;
    }

    switch (lock->error()) {
    case QLockFile::LockFailedError: {
        qint64 pid = 0;
        QString host, app;
        lock->getLockInfo(&pid, &host, &app);
        m_ownerPid = pid;
        return false; // another live instance owns this database
    }
    case QLockFile::PermissionError:
        m_lockError = QStringLiteral("Cannot create the lock file %1 (permission denied).").arg(lockFilePath);
        break;
    default:
        m_lockError = QStringLiteral("Cannot create the lock file %1.").arg(lockFilePath);
        break;
    }
    // Could not create the lock at all: do not lock the user out; the database
    // layer reports the underlying problem if it is real.
    m_primary = true;
    return true;
}

void SingleInstanceGuard::release()
{
    if (m_lock && m_lock->isLocked())
        m_lock->unlock();
    m_lock.reset();
    m_primary = false;
}

bool SingleInstanceGuard::isPrimary() const
{
    return m_primary;
}

bool SingleInstanceGuard::activateWindowOfProcess(qint64 pid)
{
#ifdef Q_OS_WIN
    if (pid <= 0)
        return false;
    FindWindowData data;
    data.pid = static_cast<DWORD>(pid);
    EnumWindows(findWindowOfProcess, reinterpret_cast<LPARAM>(&data));
    if (!data.window)
        return false;
    if (IsIconic(data.window))
        ShowWindow(data.window, SW_RESTORE);
    return SetForegroundWindow(data.window) != 0;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

bool SingleInstanceGuard::sendDataToProcess(qint64 pid, quintptr id, const QByteArray &payload)
{
#ifdef Q_OS_WIN
    if (pid <= 0)
        return false;
    FindWindowData data;
    data.pid = static_cast<DWORD>(pid);
    EnumWindows(findWindowOfProcess, reinterpret_cast<LPARAM>(&data));
    if (!data.window)
        return false;
    // This process was just started by the user (a clicked link), so it may
    // pass the right to take the foreground on to the running instance;
    // otherwise Windows would only flash that instance's taskbar button.
    AllowSetForegroundWindow(static_cast<DWORD>(pid));
    COPYDATASTRUCT cds;
    cds.dwData = static_cast<ULONG_PTR>(id);
    cds.cbData = static_cast<DWORD>(payload.size());
    cds.lpData = const_cast<char *>(payload.constData());
    DWORD_PTR answer = 0;
    // The receiver copies the payload before it returns; a hung receiver is
    // given up on after three seconds rather than blocking this process.
    const LRESULT sent = SendMessageTimeoutW(data.window, WM_COPYDATA, 0,
                                             reinterpret_cast<LPARAM>(&cds),
                                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &answer);
    return sent != 0 && answer != 0;
#else
    Q_UNUSED(pid);
    Q_UNUSED(id);
    Q_UNUSED(payload);
    return false;
#endif
}
