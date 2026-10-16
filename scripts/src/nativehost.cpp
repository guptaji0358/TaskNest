#include "nativehost.h"

#include "integrationclient.h"
#include "registryutil.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace NativeHost {

namespace {

QJsonObject okReply(const QJsonObject &result = {})
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    o.insert(QStringLiteral("result"), result);
    return o;
}

QJsonObject errorReply(const QString &code, const QString &message)
{
    QJsonObject e;
    e.insert(QStringLiteral("code"), code);
    e.insert(QStringLiteral("message"), message);
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), e);
    return o;
}

bool running(const Options &options)
{
    if (options.taskNestRunning)
        return options.taskNestRunning();
    return options.instanceLockFile.isEmpty() ? isTaskNestRunning()
                                              : isInstanceRunning(options.instanceLockFile);
}

// The state to report when there is no usable discovery file: the endpoint
// exists only while the integration is switched on in a running TaskNest.
QJsonObject notAvailable(const Options &options)
{
    if (running(options))
        return errorReply(QStringLiteral("integration_disabled"),
                          QStringLiteral("TaskNest is running, but its local integration API is "
                                         "off. Turn on \"Local integration API\" in TaskNest "
                                         "Settings."));
    return errorReply(QStringLiteral("not_running"),
                      QStringLiteral("TaskNest is not running. Start TaskNest and try again."));
}

// Stage durations of the exchange with TaskNest (numbers only: no content, no
// address, no credential). Lets a caller tell a slow start from a slow answer.
QJsonObject withTiming(QJsonObject reply, const IntegrationResponse &r)
{
    QJsonObject t;
    t.insert(QStringLiteral("connect"), static_cast<double>(r.connectMs));
    t.insert(QStringLiteral("answer"), static_cast<double>(r.answerMs));
    reply.insert(QStringLiteral("timing_ms"), t);
    if (!r.ok) // the client's own fixed vocabulary, for diagnosis
        reply.insert(QStringLiteral("client_code"), r.errorCode);
    return reply;
}

// Maps an answer from the integration endpoint to this host's vocabulary.
// `sideEffects`: the operation changes data, so "no answer" after the request
// was sent is an unknown outcome, not a failure.
QJsonObject mapFailure(const IntegrationResponse &r, const Options &options, bool sideEffects = false)
{
    const QString code = r.errorCode;
    if (sideEffects && r.stage == IntegrationResponse::Stage::RequestSent
        && (code == QLatin1String("timeout") || code == QLatin1String("disconnected")
            || code == QLatin1String("bad_reply")))
        return errorReply(QStringLiteral("outcome_unknown"),
                          QStringLiteral("TaskNest did not confirm the capture in time. It may "
                                         "or may not have been saved."));
    if (code == QLatin1String("unavailable"))
        return notAvailable(options);
    if (code == QLatin1String("unauthorized"))
        return errorReply(QStringLiteral("unauthorized"),
                          QStringLiteral("TaskNest refused the request (its integration "
                                         "credential changed). Try again."));
    if (code == QLatin1String("timeout") || code == QLatin1String("disconnected"))
        return errorReply(QStringLiteral("timeout"),
                          QStringLiteral("TaskNest did not answer in time. It may be busy; try again."));
    if (code == QLatin1String("invalid_argument") || code == QLatin1String("too_large")
        || code == QLatin1String("not_found"))
        return errorReply(code, r.errorMessage.left(200));
    return errorReply(QStringLiteral("rejected"),
                      QStringLiteral("TaskNest could not store the capture."));
}

bool hasControl(const QString &s)
{
    for (const QChar c : s) {
        if ((c.unicode() < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\r')
             && c != QLatin1Char('\t'))
            || c.unicode() == 0x7f)
            return true;
    }
    return false;
}

QString oneLine(const QString &s)
{
    QString out = s;
    out.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return out.trimmed();
}

} // namespace

QString extensionOrigin()
{
    return QStringLiteral("chrome-extension://%1/").arg(QLatin1String(kExtensionId));
}

QByteArray encodeFrame(const QByteArray &json)
{
    QByteArray frame;
    const quint32 n = static_cast<quint32>(json.size());
    frame.append(static_cast<char>(n & 0xFF));
    frame.append(static_cast<char>((n >> 8) & 0xFF));
    frame.append(static_cast<char>((n >> 16) & 0xFF));
    frame.append(static_cast<char>((n >> 24) & 0xFF));
    frame.append(json);
    return frame;
}

static bool readExactly(QIODevice &in, char *buffer, qint64 count)
{
    qint64 done = 0;
    while (done < count) {
        const qint64 n = in.read(buffer + done, count - done);
        if (n > 0) {
            done += n;
        } else if (n == 0 && in.waitForReadyRead(5000)) {
            continue;
        } else {
            return false; // EOF or error
        }
    }
    return true;
}

FrameStatus readFrame(QIODevice &in, QByteArray *payload)
{
    unsigned char head[4];
    qint64 got = 0;
    while (got < 4) {
        const qint64 n = in.read(reinterpret_cast<char *>(head) + got, 4 - got);
        if (n > 0) {
            got += n;
        } else if (n == 0 && in.waitForReadyRead(5000)) {
            continue;
        } else {
            return got == 0 ? FrameStatus::Eof : FrameStatus::Truncated;
        }
    }
    const quint32 length = static_cast<quint32>(head[0]) | (static_cast<quint32>(head[1]) << 8)
                           | (static_cast<quint32>(head[2]) << 16) | (static_cast<quint32>(head[3]) << 24);
    if (length == 0)
        return FrameStatus::Empty;
    if (length > static_cast<quint32>(kMaxRequestBytes))
        return FrameStatus::TooLarge; // nothing of the payload is read
    QByteArray data(static_cast<int>(length), Qt::Uninitialized);
    if (!readExactly(in, data.data(), data.size()))
        return FrameStatus::Truncated;
    if (payload)
        *payload = data;
    return FrameStatus::Ok;
}

bool isInstanceRunning(const QString &lockFile)
{
#ifdef Q_OS_WIN
    if (lockFile.isEmpty() || !QFileInfo::exists(lockFile))
        return false;
    qint64 pid = 0;
    QString host, application;
    if (!QLockFile(lockFile).getLockInfo(&pid, &host, &application) || pid <= 0)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return false;
    bool alive = false;
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE) {
        wchar_t image[MAX_PATH * 2];
        DWORD size = sizeof(image) / sizeof(image[0]);
        // the id may have been reused by something else: it must still be TaskNest.exe
        alive = QueryFullProcessImageNameW(process, 0, image, &size)
                && QFileInfo(QString::fromWCharArray(image, static_cast<int>(size)))
                           .fileName().compare(QStringLiteral("TaskNest.exe"), Qt::CaseInsensitive) == 0;
    }
    CloseHandle(process);
    return alive;
#else
    Q_UNUSED(lockFile);
    return false;
#endif
}

QStringList productionLockFiles()
{
    const QString lockName = QStringLiteral("tasknest.db.lock");
    QStringList files{QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data/") + lockName)};
    const QString roaming = qEnvironmentVariable("APPDATA");
    if (!roaming.isEmpty())
        files << QDir(roaming).filePath(QStringLiteral("TaskNest/") + lockName);
    return files;
}

bool isTaskNestRunning()
{
    for (const QString &file : productionLockFiles()) {
        if (isInstanceRunning(file))
            return true;
    }
    return false;
}

QJsonObject handle(const QByteArray &payload, const Options &options)
{
    if (payload.size() > kMaxRequestBytes)
        return errorReply(QStringLiteral("too_large"), QStringLiteral("The message is too large."));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return errorReply(QStringLiteral("bad_message"),
                          QStringLiteral("The message must be one JSON object."));
    const QJsonObject request = doc.object();
    if (!options.configurationError.isEmpty())
        return errorReply(QStringLiteral("configuration"),
                          QStringLiteral("The browser capture test configuration is not usable."));
    const QJsonValue opValue = request.value(QStringLiteral("op"));
    if (!opValue.isString())
        return errorReply(QStringLiteral("bad_message"), QStringLiteral("\"op\" must be a string."));
    const QString op = opValue.toString();

    auto onlyKeys = [&request](const QStringList &allowed) {
        for (auto it = request.begin(); it != request.end(); ++it) {
            if (!allowed.contains(it.key()))
                return false;
        }
        return true;
    };

    // Where to look is decided here, never by the message.
    const QString directory = options.discoveryDirectory.isEmpty()
                                  ? IntegrationClient::defaultDiscoveryDirectory()
                                  : options.discoveryDirectory;
    const QString file = IntegrationClient::newestDiscoveryFile(directory);

    if (op == QLatin1String("status")) {
        if (!onlyKeys({QStringLiteral("op")}))
            return errorReply(QStringLiteral("bad_message"), QStringLiteral("Unsupported field."));
        DiscoveryRecord record;
        if (file.isEmpty() || !IntegrationClient::readDiscovery(file, &record))
            return notAvailable(options);
        const IntegrationResponse r = IntegrationClient::request(
            QStringLiteral("ping"), {}, options.timeoutMs, file);
        if (!r.ok)
            return withTiming(mapFailure(r, options), r);
        QJsonObject result;
        result.insert(QStringLiteral("state"), QStringLiteral("ready"));
        return withTiming(okReply(result), r);
    }

    if (op == QLatin1String("capture")) {
        if (!onlyKeys({QStringLiteral("op"), QStringLiteral("title"), QStringLiteral("url"),
                       QStringLiteral("text"), QStringLiteral("request_id")}))
            return errorReply(QStringLiteral("bad_message"), QStringLiteral("Unsupported field."));
        for (const char *key : {"title", "url", "text", "request_id"}) {
            const QJsonValue v = request.value(QLatin1String(key));
            if (!v.isUndefined() && !v.isString())
                return errorReply(QStringLiteral("invalid_argument"),
                                  QStringLiteral("\"%1\" must be a string.").arg(QLatin1String(key)));
        }
        QString text = request.value(QStringLiteral("text")).toString();
        if (hasControl(text)) {
            // strip stray control characters rather than refusing a whole capture
            QString clean;
            for (const QChar c : text) {
                if (!(c.unicode() < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\r')
                      && c != QLatin1Char('\t')) && c.unicode() != 0x7f)
                    clean += c;
            }
            text = clean;
        }
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text = text.trimmed();
        if (text.isEmpty())
            return errorReply(QStringLiteral("invalid_argument"),
                              QStringLiteral("There is no text to capture."));
        if (text.size() > kMaxTextChars)
            return errorReply(QStringLiteral("invalid_argument"),
                              QStringLiteral("The selection is longer than %1 characters.")
                                  .arg(kMaxTextChars));

        QString title = oneLine(request.value(QStringLiteral("title")).toString());
        if (hasControl(title))
            return errorReply(QStringLiteral("invalid_argument"),
                              QStringLiteral("The title contains control characters."));
        if (title.isEmpty()) {
            title = oneLine(text.section(QLatin1Char('\n'), 0, 0));
            if (title.size() > 80)
                title = title.left(79).trimmed() + QChar(0x2026);
        }
        if (title.isEmpty() || title.size() > kMaxTitleChars)
            return errorReply(QStringLiteral("invalid_argument"),
                              QStringLiteral("The title must be 1 to %1 characters.").arg(kMaxTitleChars));

        const QString url = request.value(QStringLiteral("url")).toString().trimmed();
        if (url.isEmpty() || url.size() > kMaxUrlChars)
            return errorReply(QStringLiteral("invalid_argument"),
                              QStringLiteral("The page address is missing or too long."));
        const QString requestId = request.value(QStringLiteral("request_id")).toString();

        // Needs a usable endpoint from here on.
        DiscoveryRecord record;
        if (file.isEmpty() || !IntegrationClient::readDiscovery(file, &record))
            return notAvailable(options);

        QJsonObject args;
        args.insert(QStringLiteral("url"), url);        // the API validates scheme and shape
        args.insert(QStringLiteral("title"), title);
        args.insert(QStringLiteral("notes"), text);
        if (!requestId.isEmpty())
            args.insert(QStringLiteral("request_id"), requestId);
        const IntegrationResponse r = IntegrationClient::request(
            QStringLiteral("capture_page"), args, options.timeoutMs, file);
        if (!r.ok)
            return withTiming(mapFailure(r, options, /*sideEffects=*/true), r);
        QJsonObject result;
        result.insert(QStringLiteral("task_id"), r.result.value(QStringLiteral("task_id")));
        if (r.result.value(QStringLiteral("duplicate")).toBool())
            result.insert(QStringLiteral("duplicate"), true);
        return withTiming(okReply(result), r);
    }

    return errorReply(QStringLiteral("unsupported_operation"),
                      QStringLiteral("This operation is not supported."));
}

Options optionsFromArguments(const QStringList &arguments)
{
    Options options;
    bool testMode = false;
    for (int i = 1; i < arguments.size(); ++i)
        testMode = testMode || arguments.at(i) == QLatin1String(TestProfile::kOption);
    if (!testMode)
        return options; // production: browser-supplied arguments are ignored

    // Test mode: only the arguments a browser legitimately adds, plus ours.
    static const QRegularExpression parent(QStringLiteral("^--parent-window=[0-9]+$"));
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &a = arguments.at(i);
        if (a == QLatin1String(TestProfile::kOption)) {
            ++i; // its value is validated by TestProfile::fromArguments
            continue;
        }
        if (a == extensionOrigin() || parent.match(a).hasMatch())
            continue;
        options.configurationError = QStringLiteral("unexpected argument");
        return options;
    }
    const TestProfile::Profile profile = TestProfile::fromArguments(arguments, /*create=*/false);
    if (!profile.active) {
        options.configurationError = profile.error.isEmpty() ? QStringLiteral("invalid profile")
                                                              : profile.error;
        return options;
    }
    options.discoveryDirectory = profile.discoveryDirectory; // never the default
    options.instanceLockFile = QDir(profile.dataDirectory).filePath(QStringLiteral("tasknest.db.lock"));
    return options;
}

int run(QIODevice &in, QIODevice &out, const Options &options)
{
    QByteArray payload;
    QJsonObject reply;
    switch (readFrame(in, &payload)) {
    case FrameStatus::Ok:
        reply = handle(payload, options);
        break;
    case FrameStatus::TooLarge:
        reply = errorReply(QStringLiteral("too_large"), QStringLiteral("The message is too large."));
        break;
    case FrameStatus::Empty:
    case FrameStatus::Truncated:
        reply = errorReply(QStringLiteral("bad_message"), QStringLiteral("The message was incomplete."));
        break;
    case FrameStatus::Eof:
        return 1;
    }
    out.write(encodeFrame(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
    if (auto *file = qobject_cast<QFile *>(&out))
        file->flush();
    return 0;
}

} // namespace NativeHost

// ---- registration ------------------------------------------------------------

NativeHostRegistration::NativeHostRegistration(const QString &registryBase,
                                               const QString &manifestDirectory)
    : m_base(registryBase)
    , m_manifestDirectory(manifestDirectory)
{
    if (m_manifestDirectory.isEmpty()) {
        const QString local = qEnvironmentVariable("LOCALAPPDATA");
        m_manifestDirectory = QDir(local).filePath(QStringLiteral("TaskNest/native-host"));
    }
}

NativeHostRegistration NativeHostRegistration::forProfile(const TestProfile::Profile &profile)
{
    // A profile that is not active, or whose base is outside the dedicated test
    // subtree, gets a base that no browser or production code reads.
    if (!profile.active || !TestProfile::isProfileRegistryBase(profile.registryBase)) {
        NativeHostRegistration invalid(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\TaskNestTestProfiles\\invalid"),
            QDir::tempPath() + QStringLiteral("/tasknest-invalid-profile"));
        invalid.m_profileRoot = QStringLiteral("<invalid>"); // registerHost() refuses it
        return invalid;
    }
    NativeHostRegistration r(profile.registryBase, profile.manifestDirectory);
    r.m_profileRoot = profile.root;
    return r;
}

QJsonObject NativeHostRegistration::manifestFor(const QString &hostExecutablePath)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), QLatin1String(NativeHost::kHostName));
    o.insert(QStringLiteral("description"), QStringLiteral("TaskNest browser capture bridge"));
    o.insert(QStringLiteral("path"), QDir::toNativeSeparators(hostExecutablePath));
    o.insert(QStringLiteral("type"), QStringLiteral("stdio"));
    QJsonArray origins;
    origins.append(NativeHost::extensionOrigin()); // exactly one origin, never a wildcard
    o.insert(QStringLiteral("allowed_origins"), origins);
    return o;
}

QString NativeHostRegistration::manifestPath() const
{
    return QDir(m_manifestDirectory)
        .filePath(QStringLiteral("%1.json").arg(QLatin1String(NativeHost::kHostName)));
}

QString NativeHostRegistration::launcherPath() const
{
    return QDir(m_manifestDirectory)
        .filePath(QStringLiteral("%1.bat").arg(QLatin1String(NativeHost::kHostName)));
}

bool NativeHostRegistration::isSafeDirectory(const QString &directory)
{
    if (directory.isEmpty() || !QFileInfo(directory).isDir())
        return false;
    // These would be interpreted by the command processor.
    static const QString forbidden = QStringLiteral("\"%&^<>|!()\r\n");
    for (const QChar c : directory) {
        if (forbidden.contains(c))
            return false;
    }
    return true;
}

QStringList NativeHostRegistration::runtimeDirectories(const QString &hostExecutablePath)
{
    QStringList dirs;
#ifdef Q_OS_WIN
    const QString own = QDir::cleanPath(QFileInfo(hostExecutablePath).absolutePath());
    for (const wchar_t *name : {L"Qt6Core.dll", L"Qt6Network.dll", L"libstdc++-6.dll",
                                L"libgcc_s_seh-1.dll", L"libwinpthread-1.dll"}) {
        HMODULE module = GetModuleHandleW(name);
        if (!module)
            continue;
        wchar_t buffer[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(module, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
        if (n == 0 || n >= sizeof(buffer) / sizeof(buffer[0]))
            continue;
        const QString dir = QDir::cleanPath(QFileInfo(QString::fromWCharArray(buffer, static_cast<int>(n))).absolutePath());
        if (dir.compare(own, Qt::CaseInsensitive) != 0 && !dirs.contains(dir, Qt::CaseInsensitive))
            dirs << dir;
    }
#else
    Q_UNUSED(hostExecutablePath);
#endif
    return dirs;
}

QStringList NativeHostRegistration::registryKeys() const
{
    const QString name = QLatin1String(NativeHost::kHostName);
    return {m_base + QStringLiteral("\\Google\\Chrome\\NativeMessagingHosts\\") + name,
            m_base + QStringLiteral("\\Microsoft\\Edge\\NativeMessagingHosts\\") + name};
}

bool NativeHostRegistration::isRegistered(const QString &hostExecutablePath) const
{
#ifdef Q_OS_WIN
    QFile f(manifestPath());
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const QString target = QDir::fromNativeSeparators(o.value(QStringLiteral("path")).toString());
    const bool direct = target.compare(QDir::fromNativeSeparators(hostExecutablePath),
                                       Qt::CaseInsensitive) == 0;
    bool viaLauncher = false;
    if (!direct && target.compare(QDir::fromNativeSeparators(launcherPath()), Qt::CaseInsensitive) == 0) {
        QFile launcher(launcherPath());
        viaLauncher = launcher.open(QIODevice::ReadOnly)
                      && QString::fromLocal8Bit(launcher.readAll())
                             .contains(QDir::toNativeSeparators(hostExecutablePath), Qt::CaseInsensitive);
    }
    if (!direct && !viaLauncher)
        return false;
    for (const QString &key : registryKeys()) {
        QString value;
        if (!RegistryUtil::readDefault(key, &value)) // read-only: never creates a key
            return false;
        if (QDir::fromNativeSeparators(value).compare(QDir::fromNativeSeparators(manifestPath()),
                                                      Qt::CaseInsensitive) != 0)
            return false;
    }
    return true;
#else
    Q_UNUSED(hostExecutablePath);
    return false;
#endif
}

bool NativeHostRegistration::registerHost(const QString &hostExecutablePath, QString *error,
                                          const QStringList &runtimeDirectories)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
#ifdef Q_OS_WIN
    if (!QFileInfo(hostExecutablePath).isFile())
        return fail(QStringLiteral("TaskNestHost.exe was not found next to TaskNest.exe."));
    if (!m_profileRoot.isEmpty() && !TestProfile::isProfileRegistryBase(m_base))
        return fail(QStringLiteral("The test profile registry location is not valid."));
    if (!m_profileRoot.isEmpty() && !isSafeDirectory(m_profileRoot))
        return fail(QStringLiteral("The test profile folder name cannot be used by the launcher."));
    QDir().mkpath(m_manifestDirectory);
    QString target = hostExecutablePath;
    if (!runtimeDirectories.isEmpty() || !m_profileRoot.isEmpty()) {
        if (!isSafeDirectory(QFileInfo(hostExecutablePath).absolutePath()))
            return fail(QStringLiteral("The TaskNest folder name cannot be used by the launcher."));
        QStringList dirs;
        for (const QString &d : runtimeDirectories) {
            if (!isSafeDirectory(d))
                return fail(QStringLiteral("A runtime folder name cannot be used by the launcher."));
            dirs << QDir::toNativeSeparators(d);
        }
        const QString profileArgument =
            m_profileRoot.isEmpty()
                ? QString()
                : QStringLiteral(" %1 \"%2\"").arg(QLatin1String(TestProfile::kOption),
                                                   QDir::toNativeSeparators(m_profileRoot));
        const QString script = QStringLiteral("@echo off\r\nset \"PATH=%1;%PATH%\"\r\n\"%2\"%3 %*\r\n")
                                   .arg(dirs.join(QLatin1Char(';')),
                                        QDir::toNativeSeparators(hostExecutablePath), profileArgument);
        QFile launcher(launcherPath());
        if (!launcher.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || launcher.write(script.toLocal8Bit()) <= 0)
            return fail(QStringLiteral("The host launcher could not be written."));
        launcher.close();
        target = launcherPath();
    } else {
        QFile::remove(launcherPath()); // an older launcher is no longer wanted
    }
    const QByteArray manifest = QJsonDocument(manifestFor(target)).toJson(QJsonDocument::Indented);
    QSaveFile file(manifestPath());
    bool written = file.open(QIODevice::WriteOnly) && file.write(manifest) > 0 && file.commit();
    if (!written) {
        // Replacing a file a browser still has open can fail; write in place.
        QFile direct(manifestPath());
        written = direct.open(QIODevice::WriteOnly | QIODevice::Truncate)
                  && direct.write(manifest) == manifest.size();
    }
    if (!written)
        return fail(QStringLiteral("The host manifest could not be written."));
    for (const QString &key : registryKeys()) {
        QSettings s(key, QSettings::NativeFormat);
        s.setValue(QStringLiteral("Default"), QDir::toNativeSeparators(manifestPath()));
        s.sync();
        if (s.status() != QSettings::NoError)
            return fail(QStringLiteral("Windows refused to store the browser registration."));
    }
    return true;
#else
    Q_UNUSED(hostExecutablePath);
    return fail(QStringLiteral("Browser capture is only available on Windows."));
#endif
}

bool NativeHostRegistration::unregisterHost(QString *error)
{
#ifdef Q_OS_WIN
    bool ok = true;
    for (const QString &key : registryKeys())
        ok = RegistryUtil::deleteTree(key) && ok; // our own host key only
    QFile::remove(manifestPath()); // our own files only; the folder is left
    QFile::remove(launcherPath());
    if (!ok && error)
        *error = QStringLiteral("Windows refused to remove the browser registration.");
    return ok;
#else
    Q_UNUSED(error);
    return true;
#endif
}
