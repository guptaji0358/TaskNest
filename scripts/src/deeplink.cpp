#include "deeplink.h"

#include "registryutil.h"

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

DeepLink fail(const QString &why)
{
    DeepLink l;
    l.error = why;
    return l;
}

} // namespace

QStringList DeepLink::views()
{
    return {QStringLiteral("all"), QStringLiteral("today"), QStringLiteral("upcoming"),
            QStringLiteral("completed"), QStringLiteral("inbox")};
}

DeepLink DeepLink::parse(const QString &text)
{
    const QString raw = text.trimmed();
    if (raw.isEmpty() || raw.size() > kMaxLength)
        return fail(QStringLiteral("The link is empty or too long."));
    for (const QChar c : raw) {
        if (c.unicode() < 0x21 || c.unicode() == 0x7f)
            return fail(QStringLiteral("The link contains invalid characters."));
    }

    const QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QLatin1String("tasknest"), Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("This is not a TaskNest link."));
    if (!url.userInfo().isEmpty() || url.port() != -1 || url.hasQuery() || url.hasFragment())
        return fail(QStringLiteral("The link has unsupported parts."));

    const QString host = url.host().toLower();
    const QString path = url.path();

    DeepLink link;
    if (host == QLatin1String("task")) {
        static const QRegularExpression id(QStringLiteral("^/([1-9][0-9]{0,14})$"));
        const QRegularExpressionMatch m = id.match(path);
        if (!m.hasMatch())
            return fail(QStringLiteral("The task number in the link is not valid."));
        link.kind = OpenTask;
        link.taskId = m.captured(1).toLongLong();
        return link;
    }
    if (host == QLatin1String("view")) {
        const QString name = path.startsWith(QLatin1Char('/')) ? path.mid(1).toLower() : QString();
        if (!views().contains(name))
            return fail(QStringLiteral("That view is not supported."));
        link.kind = OpenView;
        link.view = name;
        return link;
    }
    if (host == QLatin1String("quickadd")) {
        if (!path.isEmpty() && path != QLatin1String("/"))
            return fail(QStringLiteral("The link has unsupported parts."));
        link.kind = QuickAdd;
        return link;
    }
    return fail(QStringLiteral("That action is not supported."));
}

QString DeepLink::fromArguments(const QStringList &arguments)
{
    // The first element is the executable itself.
    for (int i = 1; i < arguments.size(); ++i) {
        if (arguments.at(i).startsWith(QLatin1String("tasknest:"), Qt::CaseInsensitive))
            return arguments.at(i);
    }
    return QString();
}

// ---- registry ---------------------------------------------------------------

UrlSchemeRegistration::UrlSchemeRegistration(const QString &classesRoot)
    : m_root(classesRoot)
{
}

QString UrlSchemeRegistration::commandFor(const QString &exePath)
{
    return QStringLiteral("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(exePath), QStringLiteral("%1"));
}

bool UrlSchemeRegistration::isRegistered(const QString &exePath) const
{
#ifdef Q_OS_WIN
    // Read-only: asking must never create the key.
    QString command;
    if (!RegistryUtil::readDefault(m_root + QStringLiteral("\\tasknest\\shell\\open\\command"), &command))
        return false;
    return !command.isEmpty()
           && command.compare(commandFor(exePath), Qt::CaseInsensitive) == 0;
#else
    Q_UNUSED(exePath);
    return false;
#endif
}

bool UrlSchemeRegistration::registerScheme(const QString &exePath, QString *error)
{
#ifdef Q_OS_WIN
    QSettings s(m_root + QStringLiteral("\\tasknest"), QSettings::NativeFormat);
    s.setValue(QStringLiteral("Default"), QStringLiteral("URL:TaskNest Protocol"));
    s.setValue(QStringLiteral("URL Protocol"), QString());
    s.setValue(QStringLiteral("shell/open/command/Default"), commandFor(exePath));
    s.sync();
    if (s.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("Windows refused to store the link handler.");
        return false;
    }
    return true;
#else
    Q_UNUSED(exePath);
    if (error)
        *error = QStringLiteral("Link registration is only available on Windows.");
    return false;
#endif
}

bool UrlSchemeRegistration::unregisterScheme(QString *error)
{
#ifdef Q_OS_WIN
    // The whole tasknest key, including the key itself (no empty key is left).
    if (!RegistryUtil::deleteTree(m_root + QStringLiteral("\\tasknest"))) {
        if (error)
            *error = QStringLiteral("Windows refused to remove the link handler.");
        return false;
    }
    return true;
#else
    Q_UNUSED(error);
    return true;
#endif
}

// ---- router -----------------------------------------------------------------

DeepLinkRouter::DeepLinkRouter(QObject *parent)
    : QObject(parent)
{
}

bool DeepLinkRouter::handleUrl(const QString &text)
{
    const DeepLink link = DeepLink::parse(text);
    switch (link.kind) {
    case DeepLink::OpenTask:
        emit openTaskRequested(link.taskId);
        return true;
    case DeepLink::OpenView:
        emit openViewRequested(link.view);
        return true;
    case DeepLink::QuickAdd:
        emit quickAddRequested();
        return true;
    case DeepLink::Invalid:
        break;
    }
    emit rejected(link.error); // a fixed message: the link text is not repeated
    return false;
}

bool DeepLinkRouter::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG" || !message)
        return false;
    const MSG *msg = static_cast<const MSG *>(message);
    if (msg->message != WM_COPYDATA || !msg->hwnd)
        return false;
    const auto *data = reinterpret_cast<const COPYDATASTRUCT *>(msg->lParam);
    if (!data || data->dwData != kCopyDataId)
        return false; // some other WM_COPYDATA: not ours
    QString text;
    // UTF-16 payload, bounded; anything else is simply refused.
    if (data->lpData && data->cbData > 0 && data->cbData % 2 == 0
        && data->cbData <= DeepLink::kMaxLength * 2) {
        text = QString::fromUtf16(static_cast<const char16_t *>(data->lpData),
                                  static_cast<qsizetype>(data->cbData / 2));
    }
    // Answer the sender at once; the (possibly UI-opening) work runs afterwards.
    QMetaObject::invokeMethod(this, [this, text]() { handleUrl(text); }, Qt::QueuedConnection);
    if (result)
        *result = TRUE;
    return true;
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}
