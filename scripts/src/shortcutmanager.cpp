#include "shortcutmanager.h"

#include "deeplink.h"
#include "nativehost.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

QString exePath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

bool samePath(const QString &a, const QString &b)
{
    return !a.isEmpty() && QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

} // namespace

ShortcutManager::ShortcutManager(QObject *parent)
    : QObject(parent)
{
}

bool ShortcutManager::supported() const
{
#ifdef Q_OS_WIN
    return !startMenuShortcutPath().isEmpty();
#else
    return false;
#endif
}

QString ShortcutManager::startMenuShortcutPath()
{
    // %APPDATA%\Microsoft\Windows\Start Menu\Programs
    const QString programs = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    if (programs.isEmpty())
        return QString();
    return QDir(programs).filePath(QStringLiteral("TaskNest.lnk"));
}

QString ShortcutManager::pinnedTaskbarFolder()
{
    const QString roaming = qEnvironmentVariable("APPDATA");
    if (roaming.isEmpty())
        return QString();
    return QDir(roaming).filePath(
        QStringLiteral("Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"));
}

bool ShortcutManager::startMenuShortcut() const
{
    const QString path = startMenuShortcutPath();
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    // A shortcut left by a moved/other copy of the app does not count.
    return samePath(QFileInfo(path).symLinkTarget(), QCoreApplication::applicationFilePath());
}

bool ShortcutManager::taskbarPinned() const
{
    const QString folder = pinnedTaskbarFolder();
    if (folder.isEmpty())
        return false;
    const QFileInfoList links = QDir(folder).entryInfoList({QStringLiteral("*.lnk")}, QDir::Files);
    for (const QFileInfo &link : links) {
        if (samePath(link.symLinkTarget(), QCoreApplication::applicationFilePath()))
            return true;
    }
    return false;
}

bool ShortcutManager::linksRegistered() const
{
    if (m_profile.active)
        return false; // never reads or writes the real scheme registration
    return UrlSchemeRegistration().isRegistered(QCoreApplication::applicationFilePath());
}

static QString hostExecutablePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("TaskNestHost.exe"));
}

bool ShortcutManager::browserHostRegistered() const
{
    if (m_profile.active)
        return NativeHostRegistration::forProfile(m_profile).isRegistered(hostExecutablePath());
    return NativeHostRegistration().isRegistered(hostExecutablePath());
}

bool ShortcutManager::setBrowserHostRegistered(bool enabled)
{
    m_error.clear();
    NativeHostRegistration registration = m_profile.active
                                              ? NativeHostRegistration::forProfile(m_profile)
                                              : NativeHostRegistration();
    QString why;
    const bool ok = enabled ? registration.registerHost(hostExecutablePath(), &why,
                                                        NativeHostRegistration::runtimeDirectories(hostExecutablePath()))
                            : registration.unregisterHost(&why);
    if (!ok)
        m_error = why;
    emit changed();
    return ok;
}

bool ShortcutManager::setLinksRegistered(bool enabled)
{
    m_error.clear();
    if (m_profile.active) {
        m_error = tr("Links are not available in a test profile.");
        emit changed();
        return false;
    }
    UrlSchemeRegistration registration;
    QString why;
    const bool ok = enabled ? registration.registerScheme(QCoreApplication::applicationFilePath(), &why)
                            : registration.unregisterScheme(&why);
    if (!ok)
        m_error = why;
    emit changed();
    return ok;
}

bool ShortcutManager::setStartMenuShortcut(bool enabled)
{
    m_error.clear();
    if (m_profile.active) {
        m_error = tr("Shortcuts are not available in a test profile.");
        emit changed();
        return false;
    }
    const QString path = startMenuShortcutPath();
    if (!supported() || path.isEmpty()) {
        m_error = tr("Start Menu shortcuts are not available here.");
        return false;
    }

    if (enabled) {
        if (startMenuShortcut())
            return true; // already correct: nothing to duplicate
        // Replace only a shortcut with our own name (it points elsewhere or is broken).
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            m_error = tr("Could not replace the existing shortcut.");
            emit changed();
            return false;
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!QFile::link(QCoreApplication::applicationFilePath(), path)) {
            m_error = tr("Could not create the Start Menu shortcut.");
            emit changed();
            return false;
        }
    } else if (QFileInfo::exists(path) && !QFile::remove(path)) {
        m_error = tr("Could not remove the Start Menu shortcut.");
        emit changed();
        return false;
    }
    emit changed();
    return true;
}

QString ShortcutManager::requestTaskbarPin()
{
    m_error.clear();
    if (taskbarPinned())
        return tr("Already pinned. Unpin it from the taskbar with right-click → Unpin.");
    if (!setStartMenuShortcut(true))
        return QString();
#ifdef Q_OS_WIN
    // Show the shortcut so "Pin to taskbar" is one right-click away.
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,") + QDir::toNativeSeparators(startMenuShortcutPath())});
#endif
    return tr("Windows doesn't allow apps to pin themselves. Right-click TaskNest "
              "→ Pin to taskbar.");
}

void ShortcutManager::refresh()
{
    emit changed();
}
