#pragma once

#include <QString>
#include <QtGlobal>

#include <memory>

class QLockFile;

// Keeps two TaskNest processes from using the same database at once.
//
// It holds a QLockFile next to the database. QLockFile records the owner's
// process id and treats a lock whose owner is no longer running as stale, so a
// crashed or killed instance never blocks the next launch. Nothing runs in the
// background; the lock is released when the guard is destroyed or the process
// ends.
class SingleInstanceGuard
{
public:
    SingleInstanceGuard();
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard &) = delete;
    SingleInstanceGuard &operator=(const SingleInstanceGuard &) = delete;

    // True when this process may use the database: it now owns the lock, or the
    // lock file could not be created at all (permissions; the database would
    // fail the same way and say so). False only when another live process
    // holds the lock; ownerPid() then names it.
    bool acquire(const QString &lockFilePath);
    void release();

    bool isPrimary() const;
    qint64 ownerPid() const { return m_ownerPid; }
    QString lockError() const { return m_lockError; }

    // Windows: restores and focuses the visible top-level window of `pid`.
    // Returns false if there is none (or on other platforms).
    static bool activateWindowOfProcess(qint64 pid);

private:
    std::unique_ptr<QLockFile> m_lock;
    qint64 m_ownerPid = 0;
    QString m_lockError;
    bool m_primary = false;
};
