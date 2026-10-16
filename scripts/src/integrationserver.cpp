#include "integrationserver.h"

#include "task.h"
#include "tasklistmodel.h"
#include "taskrepository.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {

const QStringList kViews = {QStringLiteral("all"), QStringLiteral("today"),
                            QStringLiteral("upcoming"), QStringLiteral("completed"),
                            QStringLiteral("inbox")};

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

QByteArray toLine(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

// Constant-time comparison (the token is a secret).
bool sameSecret(const QString &a, const QString &b)
{
    const QByteArray x = a.toUtf8(), y = b.toUtf8();
    if (x.size() != y.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < x.size(); ++i)
        diff |= static_cast<unsigned char>(x[i] ^ y[i]);
    return diff == 0;
}

bool hasControlChars(const QString &s, bool allowNewlines)
{
    for (const QChar c : s) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
            if (allowNewlines && (c == QLatin1Char('\n') || c == QLatin1Char('\r') || c == QLatin1Char('\t')))
                continue;
            return true;
        }
    }
    return false;
}

// Rejects any key not in `allowed`; returns the offending key or empty.
QString unknownKey(const QJsonObject &o, const QStringList &allowed)
{
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!allowed.contains(it.key()))
            return it.key();
    }
    return QString();
}

QString hashOf(const QString &text)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

struct TextCheck
{
    bool ok = false;
    QString value;
    QString error;
};

TextCheck checkText(const QJsonObject &args, const QString &key, int maxChars, bool required,
                    bool multiline)
{
    TextCheck r;
    const QJsonValue v = args.value(key);
    if (v.isUndefined() || v.isNull()) {
        if (required)
            r.error = QStringLiteral("\"%1\" is required.").arg(key);
        else
            r.ok = true;
        return r;
    }
    if (!v.isString()) {
        r.error = QStringLiteral("\"%1\" must be a string.").arg(key);
        return r;
    }
    QString s = v.toString();
    if (hasControlChars(s, multiline)) {
        r.error = QStringLiteral("\"%1\" contains control characters.").arg(key);
        return r;
    }
    if (!multiline)
        s = s.trimmed();
    if (required && s.trimmed().isEmpty()) {
        r.error = QStringLiteral("\"%1\" must not be empty.").arg(key);
        return r;
    }
    if (s.size() > maxChars) {
        r.error = QStringLiteral("\"%1\" is longer than %2 characters.").arg(key).arg(maxChars);
        return r;
    }
    r.ok = true;
    r.value = s;
    return r;
}

} // namespace

const QString &IntegrationServer::metadataKey()
{
    static const QString key = QStringLiteral("integration_enabled");
    return key;
}

IntegrationServer::IntegrationServer(TaskListModel &model, TaskRepository &repository,
                                     const QString &databasePath, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_repository(repository)
    , m_databasePath(QDir::cleanPath(databasePath))
    , m_discoveryDirectory(defaultDiscoveryDirectory())
{
    const QString who = qEnvironmentVariable("USERNAME") + QLatin1Char('|')
                        + m_databasePath.toLower();
    m_pipeName = QStringLiteral("TaskNest-") + hashOf(who);
}

IntegrationServer::~IntegrationServer()
{
    stop();
}

QString IntegrationServer::defaultDiscoveryDirectory()
{
    return IntegrationClient::defaultDiscoveryDirectory();
}

void IntegrationServer::setDiscoveryDirectory(const QString &directory)
{
    m_discoveryDirectory = directory;
}

QString IntegrationServer::discoveryFilePath() const
{
    return QDir(m_discoveryDirectory)
        .filePath(QStringLiteral("integration-%1.json").arg(m_pipeName.mid(9)));
}

bool IntegrationServer::listening() const
{
    return m_server && m_server->isListening();
}

bool IntegrationServer::load()
{
    QString text;
    bool found = false;
    if (!m_repository.readMetadata(metadataKey(), &text, &found)) {
        emit errorOccurred(m_repository.lastError());
        return false;
    }
    m_enabled = found && text == QLatin1String("1");
    if (m_enabled && !start()) {
        m_enabled = false; // could not open; the stored choice is left as it was
        emit stateChanged();
        return false;
    }
    emit stateChanged();
    return true;
}

void IntegrationServer::setEnabled(bool enabled)
{
    if (enabled == m_enabled && enabled == listening())
        return;
    if (enabled) {
        if (!start()) {
            m_enabled = false;
            emit stateChanged();
            return;
        }
        m_enabled = true;
    } else {
        stop();
        m_enabled = false;
    }
    if (!m_repository.writeMetadata(metadataKey(), m_enabled ? QStringLiteral("1")
                                                             : QStringLiteral("0")))
        emit errorOccurred(m_repository.lastError());
    emit stateChanged();
}

bool IntegrationServer::start()
{
    if (listening())
        return true;
    // Fresh secret for every run; nothing from an earlier run is accepted.
    m_token.clear();
    for (int i = 0; i < 4; ++i)
        m_token += QStringLiteral("%1").arg(QRandomGenerator::system()->generate64(), 16, 16,
                                            QLatin1Char('0'));

    auto *server = new QLocalServer(this);
    server->setSocketOptions(QLocalServer::UserAccessOption); // current user only
    server->setMaxPendingConnections(kMaxConnections);
    QLocalServer::removeServer(m_pipeName); // stale socket from a crashed run (no-op on Windows)
    if (!server->listen(m_pipeName)) {
        const QString why = server->errorString();
        delete server;
        m_token.clear();
        emit errorOccurred(tr("The integration endpoint could not be opened: %1").arg(why));
        return false;
    }

    // Discovery file: pipe name + token, readable by the current user only.
    QDir().mkpath(m_discoveryDirectory);
    QJsonObject info;
    info.insert(QStringLiteral("protocol"), kProtocolVersion);
    info.insert(QStringLiteral("pipe"), m_pipeName);
    info.insert(QStringLiteral("token"), m_token);
    info.insert(QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid()));
    QSaveFile file(discoveryFilePath());
    bool written = file.open(QIODevice::WriteOnly)
                   && file.write(QJsonDocument(info).toJson(QJsonDocument::Compact)) > 0
                   && file.commit();
    if (!written) {
        const QString why = file.errorString();
        server->close();
        delete server;
        m_token.clear();
        emit errorOccurred(tr("The integration endpoint could not be published: %1").arg(why));
        return false;
    }
    QFile::setPermissions(discoveryFilePath(), QFile::ReadOwner | QFile::WriteOwner);

    m_server = server;
    connect(m_server, &QLocalServer::newConnection, this, &IntegrationServer::onNewConnection);
    return true;
}

void IntegrationServer::stop()
{
    if (m_server) {
        m_server->close();
        // Drop any client still connected.
        const auto sockets = m_server->findChildren<QLocalSocket *>();
        for (QLocalSocket *s : sockets) {
            s->disconnect(this);
            s->abort();
            s->deleteLater();
        }
        delete m_server;
        m_server = nullptr;
    }
    if (!m_token.isEmpty())
        QFile::remove(discoveryFilePath()); // only a file this run wrote
    m_token.clear();
    m_connections = 0;
    m_seenRequests.clear();
    m_seenOrder.clear();
}

void IntegrationServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            break;
        if (m_connections >= kMaxConnections) {
            reply(socket, toLine(errorReply(QStringLiteral("busy"),
                                            QStringLiteral("Too many connections."))));
            socket->deleteLater();
            continue;
        }
        serve(socket);
    }
}

void IntegrationServer::serve(QLocalSocket *socket)
{
    ++m_connections;
    socket->setParent(m_server);
    QPointer<QLocalSocket> guard(socket);

    auto buffer = std::make_shared<QByteArray>();
    auto done = std::make_shared<bool>(false);

    // One request per connection. A client that is silent or slow is cut off
    // by this single-shot timer (it fires once per connection; nothing polls).
    // An already answered connection (lingering until the client reads) is left alone.
    QTimer::singleShot(kClientTimeoutMs, socket, [socket, done]() {
        if (!*done && socket->state() != QLocalSocket::UnconnectedState) {
            *done = true;
            reply(socket, toLine(errorReply(QStringLiteral("timeout"),
                                            QStringLiteral("No complete request received."))));
        }
    });

    auto finish = [this, socket, buffer, done](const QByteArray &line) {
        if (*done)
            return;
        *done = true;
        reply(socket, handleLine(line));
    };

    connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer, done, finish]() {
        if (*done)
            return;
        buffer->append(socket->readAll());
        const int nl = buffer->indexOf('\n');
        if (nl >= 0) {
            if (nl > kMaxMessageBytes) {
                *done = true;
                reply(socket, toLine(errorReply(QStringLiteral("too_large"),
                                                QStringLiteral("Request is too large."))));
                return;
            }
            finish(buffer->left(nl));
        } else if (buffer->size() > kMaxMessageBytes) {
            *done = true;
            reply(socket, toLine(errorReply(QStringLiteral("too_large"),
                                            QStringLiteral("Request is too large."))));
        }
    });
    // A client that sends a request and closes without a newline is answered too.
    connect(socket, &QLocalSocket::disconnected, this, [this, socket, buffer, done]() {
        if (!*done && !buffer->isEmpty()) {
            *done = true; // the peer is gone: nobody to reply to
        }
        --m_connections;
        socket->deleteLater();
    });
}

void IntegrationServer::reply(QLocalSocket *socket, const QByteArray &line)
{
    if (socket->state() != QLocalSocket::ConnectedState) {
        socket->disconnectFromServer();
        return;
    }
    socket->write(line);
    socket->flush();
    // Closing the pipe right after writing can discard an answer the client has
    // not read yet (seen under load: the task was stored, the answer was lost).
    // The client closes its end once it has read the line, which ends the
    // connection; this single-shot bound only covers a client that never does.
    QPointer<QLocalSocket> guard(socket);
    QTimer::singleShot(kReplyLingerMs, socket, [guard]() {
        if (guard)
            guard->disconnectFromServer();
    });
}

QByteArray IntegrationServer::handleLine(const QByteArray &line)
{
    if (line.size() > kMaxMessageBytes)
        return toLine(errorReply(QStringLiteral("too_large"), QStringLiteral("Request is too large.")));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return toLine(errorReply(QStringLiteral("bad_request"),
                                 QStringLiteral("The request must be one JSON object.")));
    return toLine(handle(doc.object()));
}

QJsonObject IntegrationServer::handle(const QJsonObject &request)
{
    if (!listening() && m_token.isEmpty())
        return errorReply(QStringLiteral("unavailable"), QStringLiteral("The integration is off."));

    // Authorization first, so an unauthenticated client learns nothing else.
    const QJsonValue token = request.value(QStringLiteral("token"));
    if (!token.isString() || !sameSecret(token.toString(), m_token))
        return errorReply(QStringLiteral("unauthorized"), QStringLiteral("Missing or wrong token."));

    const QString bad = unknownKey(request, {QStringLiteral("v"), QStringLiteral("token"),
                                             QStringLiteral("op"), QStringLiteral("args")});
    if (!bad.isEmpty())
        return errorReply(QStringLiteral("unknown_field"),
                          QStringLiteral("Unsupported field \"%1\".").arg(bad));

    const QJsonValue version = request.value(QStringLiteral("v"));
    if (!version.isDouble() || version.toInt() != kProtocolVersion)
        return errorReply(QStringLiteral("unsupported_version"),
                          QStringLiteral("Protocol version %1 is required.").arg(kProtocolVersion));

    const QJsonValue op = request.value(QStringLiteral("op"));
    if (!op.isString())
        return errorReply(QStringLiteral("bad_request"), QStringLiteral("\"op\" must be a string."));

    QJsonObject args;
    const QJsonValue a = request.value(QStringLiteral("args"));
    if (!a.isUndefined()) {
        if (!a.isObject())
            return errorReply(QStringLiteral("bad_request"),
                              QStringLiteral("\"args\" must be an object."));
        args = a.toObject();
    }

    const QString name = op.toString();
    if (name == QLatin1String("ping")) {
        const QString extra = unknownKey(args, {});
        if (!extra.isEmpty())
            return errorReply(QStringLiteral("unknown_field"),
                              QStringLiteral("Unsupported field \"%1\".").arg(extra));
        QJsonObject r;
        r.insert(QStringLiteral("app"), QStringLiteral("TaskNest"));
        r.insert(QStringLiteral("protocol"), kProtocolVersion);
        return okReply(r);
    }
    if (name == QLatin1String("create_task"))
        return createTask(args);
    if (name == QLatin1String("capture_page"))
        return capturePage(args);
    if (name == QLatin1String("open_view"))
        return openView(args);
    if (name == QLatin1String("open_task"))
        return openTask(args);
    if (name == QLatin1String("quick_add"))
        return quickAdd(args);
    return errorReply(QStringLiteral("unknown_operation"),
                      QStringLiteral("Unsupported operation \"%1\".").arg(name.left(40)));
}

QJsonObject IntegrationServer::createTask(const QJsonObject &args)
{
    const QString extra = unknownKey(args, {QStringLiteral("title"), QStringLiteral("notes")});
    if (!extra.isEmpty())
        return errorReply(QStringLiteral("unknown_field"),
                          QStringLiteral("Unsupported field \"%1\".").arg(extra));
    const TextCheck title = checkText(args, QStringLiteral("title"), kMaxTitleChars, true, false);
    if (!title.ok)
        return errorReply(QStringLiteral("invalid_argument"), title.error);
    const TextCheck notes = checkText(args, QStringLiteral("notes"), kMaxNotesChars, false, true);
    if (!notes.ok)
        return errorReply(QStringLiteral("invalid_argument"), notes.error);

    if (!m_model.addTask(title.value, notes.value))
        return errorReply(QStringLiteral("internal_error"), m_model.lastError());
    QJsonObject r;
    r.insert(QStringLiteral("task_id"), static_cast<double>(m_model.lastAddedId()));
    return okReply(r);
}

QJsonObject IntegrationServer::capturePage(const QJsonObject &args)
{
    const QString extra = unknownKey(args, {QStringLiteral("url"), QStringLiteral("title"),
                                            QStringLiteral("notes"), QStringLiteral("request_id")});
    if (!extra.isEmpty())
        return errorReply(QStringLiteral("unknown_field"),
                          QStringLiteral("Unsupported field \"%1\".").arg(extra));

    const TextCheck urlText = checkText(args, QStringLiteral("url"), kMaxUrlChars, true, false);
    if (!urlText.ok)
        return errorReply(QStringLiteral("invalid_argument"), urlText.error);
    const QUrl url(urlText.value, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || (scheme != QLatin1String("http")
                                                   && scheme != QLatin1String("https"))
        || urlText.value.contains(QLatin1Char(' ')))
        return errorReply(QStringLiteral("invalid_argument"),
                          QStringLiteral("\"url\" must be a valid http or https address."));

    const TextCheck title = checkText(args, QStringLiteral("title"), kMaxTitleChars, false, false);
    if (!title.ok)
        return errorReply(QStringLiteral("invalid_argument"), title.error);
    const TextCheck notes = checkText(args, QStringLiteral("notes"), kMaxNotesChars, false, true);
    if (!notes.ok)
        return errorReply(QStringLiteral("invalid_argument"), notes.error);

    // Optional idempotency key: a retry of the *same* request must not create a
    // second task. Different (or absent) keys are never merged.
    QString requestId;
    const QJsonValue rid = args.value(QStringLiteral("request_id"));
    if (!rid.isUndefined()) {
        static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{8,64}$"));
        if (!rid.isString() || !pattern.match(rid.toString()).hasMatch())
            return errorReply(QStringLiteral("invalid_argument"),
                              QStringLiteral("\"request_id\" must be 8 to 64 letters, digits, - or _."));
        requestId = rid.toString();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const auto seen = m_seenRequests.constFind(requestId);
        if (seen != m_seenRequests.constEnd() && now - seen->at <= kRequestIdWindowSeconds) {
            QJsonObject r;
            r.insert(QStringLiteral("task_id"), static_cast<double>(seen->taskId));
            r.insert(QStringLiteral("duplicate"), true);
            return okReply(r);
        }
    }

    const QString taskTitle = title.value.isEmpty() ? url.host() + url.path() : title.value;
    QString body = notes.value.trimmed();
    if (!body.isEmpty())
        body += QStringLiteral("\n\n");
    body += QStringLiteral("Source: ") + urlText.value; // no schema change: kept in the notes

    if (!m_model.addTask(taskTitle.left(kMaxTitleChars), body))
        return errorReply(QStringLiteral("internal_error"), m_model.lastError());
    if (!requestId.isEmpty()) {
        // An id seen before (its window had expired) is moved to the newest
        // position; leaving the old entry would let the eviction below remove
        // the fresh record early.
        m_seenOrder.removeAll(requestId);
        m_seenRequests.insert(requestId, {m_model.lastAddedId(), QDateTime::currentSecsSinceEpoch()});
        m_seenOrder.append(requestId);
        while (m_seenOrder.size() > kRememberedRequestIds)
            m_seenRequests.remove(m_seenOrder.takeFirst());
    }
    QJsonObject r;
    r.insert(QStringLiteral("task_id"), static_cast<double>(m_model.lastAddedId()));
    return okReply(r);
}

QJsonObject IntegrationServer::openView(const QJsonObject &args)
{
    const QString extra = unknownKey(args, {QStringLiteral("view")});
    if (!extra.isEmpty())
        return errorReply(QStringLiteral("unknown_field"),
                          QStringLiteral("Unsupported field \"%1\".").arg(extra));
    const QJsonValue v = args.value(QStringLiteral("view"));
    if (!v.isString() || !kViews.contains(v.toString()))
        return errorReply(QStringLiteral("invalid_argument"),
                          QStringLiteral("\"view\" must be one of: all, today, upcoming, "
                                         "completed, inbox."));
    emit openViewRequested(v.toString());
    QJsonObject r;
    r.insert(QStringLiteral("view"), v.toString());
    return okReply(r);
}

QJsonObject IntegrationServer::openTask(const QJsonObject &args)
{
    const QString extra = unknownKey(args, {QStringLiteral("task_id")});
    if (!extra.isEmpty())
        return errorReply(QStringLiteral("unknown_field"),
                          QStringLiteral("Unsupported field \"%1\".").arg(extra));
    const QJsonValue v = args.value(QStringLiteral("task_id"));
    const double d = v.toDouble(-1);
    if (!v.isDouble() || d < 1 || d > 9007199254740991.0 || d != static_cast<double>(static_cast<qint64>(d)))
        return errorReply(QStringLiteral("invalid_argument"),
                          QStringLiteral("\"task_id\" must be a positive integer."));
    const qint64 id = static_cast<qint64>(d);
    Task task;
    if (!m_repository.getTask(id, &task)) {
        if (m_repository.lastError().contains(QLatin1String("was not found")))
            return errorReply(QStringLiteral("not_found"), QStringLiteral("No task with that id."));
        return errorReply(QStringLiteral("internal_error"), m_repository.lastError());
    }
    emit openTaskRequested(id);
    QJsonObject r;
    r.insert(QStringLiteral("task_id"), static_cast<double>(id));
    return okReply(r);
}

QJsonObject IntegrationServer::quickAdd(const QJsonObject &args)
{
    const QString extra = unknownKey(args, {});
    if (!extra.isEmpty())
        return errorReply(QStringLiteral("unknown_field"),
                          QStringLiteral("Unsupported field \"%1\".").arg(extra));
    emit quickAddRequested();
    return okReply();
}
