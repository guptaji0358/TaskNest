#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "testprofile.h"

class QIODevice;

// Chrome / Edge Native Messaging bridge between the TaskNest browser
// extension and the local integration endpoint (see INTEGRATION.md and
// browser-extension/README.md). The browser starts the host once per message
// (chrome.runtime.sendNativeMessage), the host answers once and exits.
//
// Boundaries (all enforced here, not in the extension):
//  * only two operations are accepted: "status" and "capture";
//  * the pipe name and credential come only from the per-user discovery file
//    and are validated before use; nothing from the message selects a pipe,
//    a file or a program;
//  * the integration credential is never part of a response;
//  * TaskNest is never started by the host.
namespace NativeHost {

constexpr const char *kHostName = "com.tasknest.host";
// Chrome/Edge derive this from the "key" in browser-extension/manifest.json.
constexpr const char *kExtensionId = "fghdblbielgdmgnmodfhdifeiggaglcj";

constexpr int kMaxRequestBytes = 64 * 1024; // whole message payload
constexpr int kMaxTextChars = 10000;
constexpr int kMaxTitleChars = 500;
constexpr int kMaxUrlChars = 2048;

QString extensionOrigin(); // "chrome-extension://<id>/"

// Native messaging framing: 4-byte little-endian length, then UTF-8 JSON.
QByteArray encodeFrame(const QByteArray &json);

enum class FrameStatus { Ok, Eof, Empty, Truncated, TooLarge };
// Reads one frame (blocking). Never reads more than kMaxRequestBytes of payload.
FrameStatus readFrame(QIODevice &in, QByteArray *payload);

struct Options
{
    QString discoveryDirectory;   // empty = the per-user default
    int timeoutMs = 3000;         // connect / answer limit
    // Lets tests decide "TaskNest.exe is running"; null = ask Windows.
    bool (*taskNestRunning)() = nullptr;
    // Non-empty: the host was started with an unusable test-profile
    // configuration; every request is answered with an error and no endpoint
    // (normal or scratch) is contacted.
    QString configurationError;
    // The single-instance lock file of the one TaskNest this host serves
    // ("<database>.lock"). Set for a test profile. Empty: the production
    // locations (see isInstanceRunning()).
    QString instanceLockFile;
};

// Builds the options from the host's command line. Without "--test-profile"
// the options are the production defaults (browser-supplied arguments such as
// the extension origin are ignored). With it, the profile must already exist
// and carry TaskNest's marker; its discovery folder is then the ONLY place the
// host looks. Anything unexpected fails closed through configurationError.
Options optionsFromArguments(const QStringList &arguments);

// True if the TaskNest that owns `lockFile` (a "<database>.lock" written by the
// single-instance guard) is alive: the lock names a process id, and that process
// must still be a TaskNest.exe. Identity comes from the database, so another
// TaskNest on a different database (a scratch profile) is never mistaken for it.
// Reads the lock file only.
bool isInstanceRunning(const QString &lockFile);

// The lock files isTaskNestRunning() consults: never a test profile's.
QStringList productionLockFiles();

// Production: the instance on the portable database next to this program or on
// the per-user one (%APPDATA%\TaskNest). Does not scan other processes.
bool isTaskNestRunning();

// Validates and executes one request payload; always returns a reply object
// {"ok":true,"result":{...}} or {"ok":false,"error":{"code","message"}}.
QJsonObject handle(const QByteArray &payload, const Options &options = Options());

// Reads one frame from `in`, writes one frame to `out`. Returns the process
// exit code (0 = a reply was written, 1 = nothing could be read).
int run(QIODevice &in, QIODevice &out, const Options &options = Options());

} // namespace NativeHost

// Registration of the host with Chrome and Edge, under HKEY_CURRENT_USER only
// and only for this one extension id.
class NativeHostRegistration
{
public:
    // `registryBase` / `manifestDirectory` are overridable for tests, so they
    // never touch the real registry keys or folders.
    explicit NativeHostRegistration(
        const QString &registryBase = QStringLiteral("HKEY_CURRENT_USER\\Software"),
        const QString &manifestDirectory = QString());

    // The host manifest JSON the browsers read.
    static QJsonObject manifestFor(const QString &hostExecutablePath);

    // Registration for a validated scratch test profile: its own registry
    // subtree and manifest folder, and a launcher that passes the profile to
    // the host as "--test-profile <folder>" (a command line TaskNest wrote, not
    // something a browser message can supply). Production code never calls it.
    static NativeHostRegistration forProfile(const TestProfile::Profile &profile);

    QString manifestPath() const;
    // The small launcher written next to the manifest when the host's Qt
    // runtime is not in its own folder (see registerHost()).
    QString launcherPath() const;
    QStringList registryKeys() const; // Chrome and Edge keys

    // Folders of the Qt / MinGW runtime DLLs this process has loaded, other
    // than the host's own folder: what the host would need on its PATH.
    static QStringList runtimeDirectories(const QString &hostExecutablePath);
    // Directory strings that are safe to put into the launcher script.
    static bool isSafeDirectory(const QString &directory);
    bool isRegistered(const QString &hostExecutablePath) const;
    // With `runtimeDirectories` the manifest points at a launcher script that
    // prepends them to PATH for the host (browsers start hosts with their own
    // environment, not the shell's). Nothing is copied or bundled.
    bool registerHost(const QString &hostExecutablePath, QString *error = nullptr,
                      const QStringList &runtimeDirectories = QStringList());
    bool unregisterHost(QString *error = nullptr);

private:
    QString m_base;
    QString m_manifestDirectory;
    QString m_profileRoot; // non-empty only for forProfile()
};
