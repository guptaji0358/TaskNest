#pragma once

#include "integrationclient.h"

#include <QJsonObject>
#include <QObject>
#include <QHash>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;
class TaskListModel;
class TaskRepository;

// Local integration endpoint (see INTEGRATION.md for the full contract).
//
//  * Transport: a Windows named pipe via QLocalServer, restricted to the
//    current user, one JSON request line per connection, one JSON reply line.
//  * Authorization: every request carries a random per-run token that is
//    written, together with the pipe name, to a discovery file in the user's
//    local application-data folder. Requests without the token are refused.
//  * Off by default. The user enables it in Settings; the setting is stored in
//    app_metadata ("integration_enabled"). While disabled there is no pipe, no
//    token and no discovery file.
//  * Operations reuse the task model (the same path as the UI), so reminders,
//    cleanup rules and the widget views stay consistent. UI actions are
//    requested through signals; the endpoint never touches windows itself.
class IntegrationServer : public QObject
{
    Q_OBJECT
    // The persisted user choice; assigning it starts or stops the endpoint.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged)
    // True only while the pipe is really open.
    Q_PROPERTY(bool listening READ listening NOTIFY stateChanged)

public:
    static constexpr int kProtocolVersion = IntegrationClient::kProtocolVersion;
    static constexpr int kMaxRequestIdChars = 64;
    static constexpr int kRememberedRequestIds = 128;
    static constexpr int kRequestIdWindowSeconds = 600;
    static constexpr int kMaxMessageBytes = 16 * 1024;   // one request line
    static constexpr int kMaxTitleChars = 500;
    static constexpr int kMaxNotesChars = 10000;
    static constexpr int kMaxUrlChars = 2048;
    static constexpr int kClientTimeoutMs = 5000;        // idle/slow client cut-off
    static constexpr int kReplyLingerMs = 3000;          // answered connection stays open for the client to read
    static constexpr int kMaxConnections = 16;
    static const QString &metadataKey();

    // `model` and `repository` must outlive the server.
    IntegrationServer(TaskListModel &model, TaskRepository &repository,
                      const QString &databasePath, QObject *parent = nullptr);
    ~IntegrationServer() override;

    // Reads the saved choice and starts the endpoint when it is on.
    bool load();

    bool enabled() const { return m_enabled; }
    bool listening() const;
    void setEnabled(bool enabled);

    // Closes the pipe and removes the discovery file (idempotent).
    Q_INVOKABLE void stop();

    QString endpointName() const { return m_pipeName; }
    QString discoveryFilePath() const;
    QString token() const { return m_token; }

    // The folder that holds the discovery file; tests point it at a scratch
    // folder before load()/setEnabled().
    void setDiscoveryDirectory(const QString &directory);
    static QString defaultDiscoveryDirectory();

    // Pure request handling (no sockets): validates `request` and runs it.
    // Returns the complete reply object.
    QJsonObject handle(const QJsonObject &request);
    // Parses one raw line and handles it; always returns a reply line.
    QByteArray handleLine(const QByteArray &line);

signals:
    void stateChanged();
    void errorOccurred(const QString &message);
    // UI requests. `view` is one of: all, today, upcoming, completed, inbox.
    void openViewRequested(const QString &view);
    void openTaskRequested(qint64 taskId);
    void quickAddRequested();

private:
    bool start();
    void onNewConnection();
    void serve(QLocalSocket *socket);
    static void reply(QLocalSocket *socket, const QByteArray &line);

    QJsonObject createTask(const QJsonObject &args);
    QJsonObject capturePage(const QJsonObject &args);
    QJsonObject openView(const QJsonObject &args);
    QJsonObject openTask(const QJsonObject &args);
    QJsonObject quickAdd(const QJsonObject &args);

    TaskListModel &m_model;
    TaskRepository &m_repository;
    QString m_databasePath;
    QString m_pipeName;
    QString m_discoveryDirectory;
    QString m_token;
    QLocalServer *m_server = nullptr;
    bool m_enabled = false;
    int m_connections = 0;
    // capture_page request_id -> {task id, when}: a retry of the same request
    // returns the first task instead of creating a second. In memory only;
    // limited to the last 128 ids seen within 10 minutes.
    struct SeenRequest { qint64 taskId; qint64 at; };
    QHash<QString, SeenRequest> m_seenRequests;
    QStringList m_seenOrder;
};
