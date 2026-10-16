#include "integrationclient.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>

namespace {

IntegrationResponse failure(const QString &code, const QString &message)
{
    IntegrationResponse r;
    r.errorCode = code;
    r.errorMessage = message;
    return r;
}

QByteArray toLine(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

IntegrationResponse parseReply(const QByteArray &line)
{
    IntegrationResponse r;
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject())
        return failure(QStringLiteral("bad_reply"), QStringLiteral("The reply was not valid JSON."));
    const QJsonObject o = doc.object();
    r.ok = o.value(QStringLiteral("ok")).toBool();
    r.result = o.value(QStringLiteral("result")).toObject();
    const QJsonObject e = o.value(QStringLiteral("error")).toObject();
    r.errorCode = e.value(QStringLiteral("code")).toString();
    r.errorMessage = e.value(QStringLiteral("message")).toString();
    return r;
}

} // namespace

QString IntegrationClient::defaultDiscoveryDirectory()
{
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (local.isEmpty())
        return QString();
    return QDir(local).filePath(QStringLiteral("TaskNest/integration"));
}

bool IntegrationClient::isValidPipeName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^TaskNest-[0-9a-f]{16}$"));
    return re.match(name).hasMatch();
}

bool IntegrationClient::isValidToken(const QString &token)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(token).hasMatch();
}

bool IntegrationClient::readDiscovery(const QString &file, DiscoveryRecord *record, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    QFileInfo info(file);
    if (file.isEmpty() || !info.isFile())
        return fail(QStringLiteral("There is no discovery file."));
    if (info.size() <= 0 || info.size() > kMaxDiscoveryFileBytes)
        return fail(QStringLiteral("The discovery file has an unexpected size."));
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("The discovery file cannot be read."));
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("The discovery file is not valid JSON."));
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("protocol")).toInt(-1) != kProtocolVersion)
        return fail(QStringLiteral("The discovery file is for another protocol version."));
    const QString pipe = o.value(QStringLiteral("pipe")).toString();
    const QString token = o.value(QStringLiteral("token")).toString();
    if (!isValidPipeName(pipe))
        return fail(QStringLiteral("The discovery file names an invalid endpoint."));
    if (!isValidToken(token))
        return fail(QStringLiteral("The discovery file has an invalid credential."));
    if (record) {
        record->pipe = pipe;
        record->token = token;
    }
    return true;
}

QString IntegrationClient::newestDiscoveryFile(const QString &directory)
{
    if (directory.isEmpty())
        return QString();
    const QDir dir(directory);
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("integration-*.json")},
                                                  QDir::Files, QDir::Time);
    return files.isEmpty() ? QString() : files.first().absoluteFilePath();
}

IntegrationResponse IntegrationClient::sendRaw(const QByteArray &line, const QString &discoveryFile,
                                               int timeoutMs)
{
    DiscoveryRecord record;
    QString why;
    if (!readDiscovery(discoveryFile, &record, &why))
        return failure(QStringLiteral("unavailable"),
                       QStringLiteral("TaskNest is not running or the integration is off."));

    QElapsedTimer clock;
    clock.start();
    QLocalSocket socket;
    socket.connectToServer(record.pipe);
    if (!socket.waitForConnected(timeoutMs)) {
        IntegrationResponse r = failure(QStringLiteral("unavailable"),
                                        QStringLiteral("TaskNest is not accepting connections."));
        r.connectMs = clock.elapsed();
        return r;
    }
    const qint64 connectMs = clock.elapsed();
    QElapsedTimer answerClock;
    answerClock.start();
    // From the first write on, the server may act on the request even if no
    // answer ever arrives, so every failure below is an uncertain outcome.
    auto uncertain = [&](const QString &code, const QString &message) {
        IntegrationResponse r = failure(code, message);
        r.stage = IntegrationResponse::Stage::RequestSent;
        r.connectMs = connectMs;
        r.answerMs = answerClock.elapsed();
        return r;
    };
    socket.write(line);
    if (!socket.waitForBytesWritten(timeoutMs))
        return uncertain(QStringLiteral("timeout"), QStringLiteral("The request could not be sent in time."));
    QByteArray data;
    while (!data.contains('\n')) {
        // The server closes its end right after writing the answer. On Windows
        // waitForReadyRead() can then return false although the answer is
        // already buffered, so whatever arrived is always collected first.
        if (socket.bytesAvailable() <= 0 && !socket.waitForReadyRead(timeoutMs)) {
            data += socket.readAll();
            break;
        }
        data += socket.readAll();
        if (data.size() > 65536)
            return uncertain(QStringLiteral("bad_reply"), QStringLiteral("The reply was too large."));
    }
    if (!data.contains('\n')) {
        // Connected but no complete answer: the peer went away or was too slow.
        return uncertain(data.isEmpty() ? QStringLiteral("timeout") : QStringLiteral("disconnected"),
                         QStringLiteral("TaskNest did not complete the answer."));
    }
    IntegrationResponse r = parseReply(data.left(data.indexOf('\n')));
    r.stage = IntegrationResponse::Stage::Answered;
    r.connectMs = connectMs;
    r.answerMs = answerClock.elapsed();
    return r;
}

IntegrationResponse IntegrationClient::request(const QString &op, const QJsonObject &args,
                                               int timeoutMs, const QString &discoveryFile,
                                               const QString &directory)
{
    const QString file = !discoveryFile.isEmpty()
                             ? discoveryFile
                             : newestDiscoveryFile(directory.isEmpty() ? defaultDiscoveryDirectory()
                                                                       : directory);
    DiscoveryRecord record;
    if (!readDiscovery(file, &record))
        return failure(QStringLiteral("unavailable"),
                       QStringLiteral("TaskNest is not running or the integration is off."));

    QJsonObject req;
    req.insert(QStringLiteral("v"), kProtocolVersion);
    req.insert(QStringLiteral("token"), record.token);
    req.insert(QStringLiteral("op"), op);
    req.insert(QStringLiteral("args"), args);
    return sendRaw(toLine(req), file, timeoutMs);
}
