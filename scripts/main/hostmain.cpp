// TaskNestHost.exe - Native Messaging host for the TaskNest browser extension.
//
// Started by Chrome/Edge (one process per message): reads one framed JSON
// request on stdin, answers with one framed JSON reply on stdout and exits.
// It does not start TaskNest, keeps nothing running and takes no command-line
// options of its own (only the launcher TaskNest writes for a scratch test
// profile adds --test-profile, see testprofile.h). The work is in scripts/src/nativehost.cpp.

#include "nativehost.h"

#include <QCoreApplication>
#include <QFile>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);  // the frames are binary
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    const NativeHost::Options options = NativeHost::optionsFromArguments(app.arguments());

    std::atomic<int> exitCode{-1};
    std::thread worker([&exitCode, &options]() {
        QFile in;
        QFile out;
        // Unbuffered: the browser keeps stdin open while it waits for the reply,
        // so a buffered read of a whole chunk would block until the pipe closes.
        if (!in.open(stdin, QIODevice::ReadOnly | QIODevice::Unbuffered)
            || !out.open(stdout, QIODevice::WriteOnly)) {
            exitCode = 1;
            return;
        }
        exitCode = NativeHost::run(in, out, options);
    });

    // A stuck stdin or an unresponsive TaskNest must not leave this process
    // lingering: give up after 12 seconds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (exitCode.load() < 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (exitCode.load() < 0) {
        std::fflush(stdout);
        std::_Exit(3);
    }
    worker.join();
    return exitCode.load();
}
