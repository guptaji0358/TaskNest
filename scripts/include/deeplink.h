#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <QStringList>

// tasknest:// links (see INTEGRATION.md).
//
//   tasknest://task/<id>                  open that task
//   tasknest://view/<name>                all | today | upcoming | completed | inbox
//   tasknest://quickadd                   open the compact Quick Add window
//
// Nothing else is accepted: no query, fragment, credentials, port or extra
// path segments. Links never carry task content or the integration token.
struct DeepLink
{
    enum Kind { Invalid, OpenTask, OpenView, QuickAdd };

    Kind kind = Invalid;
    qint64 taskId = 0;
    QString view;
    QString error; // short reason; never echoes the input

    bool isValid() const { return kind != Invalid; }

    static constexpr int kMaxLength = 256;
    static DeepLink parse(const QString &text);
    // The first command-line argument that looks like a tasknest: link
    // (Windows passes the clicked link as one argument), or an empty string.
    static QString fromArguments(const QStringList &arguments);
    static QStringList views();
};

// Windows registration of the tasknest scheme under HKEY_CURRENT_USER (no
// administrator rights). Changes only happen when the user asks in Settings.
class UrlSchemeRegistration
{
public:
    // `classesRoot` is overridable so tests never touch the real registry key.
    explicit UrlSchemeRegistration(
        const QString &classesRoot = QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"));

    // True when the scheme's open command launches exactly `exePath`.
    bool isRegistered(const QString &exePath) const;
    bool registerScheme(const QString &exePath, QString *error = nullptr);
    bool unregisterScheme(QString *error = nullptr);
    static QString commandFor(const QString &exePath);

private:
    QString m_root;
};

// Receives links forwarded by a second TaskNest launch (WM_COPYDATA to the
// main window) and links given on the command line of the first launch.
// Every link is validated by DeepLink::parse before anything happens.
class DeepLinkRouter : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    static constexpr quintptr kCopyDataId = 0x544E4B4C; // "TNKL"

    explicit DeepLinkRouter(QObject *parent = nullptr);

    // Parses and dispatches; false when the link is not valid.
    bool handleUrl(const QString &text);

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void openViewRequested(const QString &view);
    void openTaskRequested(qint64 taskId);
    void quickAddRequested();
    void rejected(const QString &reason);
};
