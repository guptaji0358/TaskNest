#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Client side of the local integration endpoint (see INTEGRATION.md). Needs
// only Qt Core and Qt Network, so the native-messaging host can use it
// without the rest of the application.
//
// It never starts TaskNest: when there is no endpoint the answer is the error
// code "unavailable" (nothing was sent: the request definitely did not run);
// when the request was written but no complete answer came back it is
// "timeout" or "disconnected" (the request may or may not have run).
struct IntegrationResponse
{
    // How far a request got. Anything from RequestSent on means the server
    // may have acted even though no answer arrived.
    enum class Stage { NotSent, RequestSent, Answered };

    bool ok = false;
    QJsonObject result;
    QString errorCode;
    QString errorMessage;
    Stage stage = Stage::NotSent;
    qint64 connectMs = -1;  // connecting to the pipe
    qint64 answerMs = -1;   // writing the request until the answer (or giving up)
};

// What the discovery file may contain after validation. The token is a
// secret: it is only ever used to build the request and is never part of a
// response, error message or log line.
struct DiscoveryRecord
{
    QString pipe;
    QString token;
};

class IntegrationClient
{
public:
    static constexpr int kProtocolVersion = 1;
    static constexpr qint64 kMaxDiscoveryFileBytes = 4096;

    // %LOCALAPPDATA%\TaskNest\integration (per user; computed, not configurable
    // from outside the process).
    static QString defaultDiscoveryDirectory();

    // Strict shapes: "TaskNest-" + 16 lower-case hex digits; 64 hex digits.
    static bool isValidPipeName(const QString &name);
    static bool isValidToken(const QString &token);

    // Reads and validates one discovery file. On failure `error` is a short
    // fixed description (never the file's content).
    static bool readDiscovery(const QString &file, DiscoveryRecord *record, QString *error = nullptr);
    // Newest integration-*.json in `directory`, or an empty string.
    static QString newestDiscoveryFile(const QString &directory);

    // `discoveryFile` empty = the newest file in `directory` (default: the
    // per-user folder). Must not be called from the thread running the server.
    static IntegrationResponse request(const QString &op, const QJsonObject &args = {},
                                       int timeoutMs = 3000,
                                       const QString &discoveryFile = QString(),
                                       const QString &directory = QString());
    // Sends a pre-built line verbatim (for protocol tests).
    static IntegrationResponse sendRaw(const QByteArray &line, const QString &discoveryFile,
                                       int timeoutMs = 3000);
};
