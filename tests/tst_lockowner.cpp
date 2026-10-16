// Test helper for tst_tasknest: holds a QLockFile (the format the single-instance
// guard writes) and keeps running until its stdin closes. It touches nothing
// but the lock file it is given. The test copies this program to a file named
// TaskNest.exe to stand in for an owning instance, and runs it under its own
// name to stand in for an unrelated live process.
//
//   tst_lockowner <lock file>     prints "ready" once locked, "failed" otherwise

#include <QCoreApplication>
#include <QLockFile>

#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 2) {
        std::puts("failed");
        return 2;
    }
    QLockFile lock(args.at(1));
    lock.setStaleLockTime(0); // never treated as stale while this process lives
    if (!lock.tryLock(0)) {
        std::puts("failed");
        std::fflush(stdout);
        return 1;
    }
    std::puts("ready");
    std::fflush(stdout);
    while (std::getchar() != EOF) {
    }
    lock.unlock();
    return 0;
}
