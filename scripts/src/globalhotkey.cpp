#include "globalhotkey.h"

#include "taskrepository.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

int nextHotkeyId()
{
    static int id = 0x4E51; // arbitrary, unique per registration in this process
    return id++;
}

#ifdef Q_OS_WIN
constexpr unsigned kCtrl = MOD_CONTROL;
constexpr unsigned kAlt = MOD_ALT;
constexpr unsigned kShift = MOD_SHIFT;
constexpr unsigned kWin = MOD_WIN;
#else
constexpr unsigned kCtrl = 2, kAlt = 1, kShift = 4, kWin = 8;
#endif

} // namespace

const QString &GlobalHotkey::metadataKey()
{
    static const QString key = QStringLiteral("global_quick_add");
    return key;
}

QStringList GlobalHotkey::defaultChoices()
{
    return {QStringLiteral("Ctrl+Alt+N"), QStringLiteral("Ctrl+Alt+T"),
            QStringLiteral("Ctrl+Alt+Q"), QStringLiteral("Ctrl+Alt+Space"),
            QStringLiteral("Ctrl+Shift+Space")};
}

bool GlobalHotkey::parse(const QString &text, unsigned *modifiers, unsigned *virtualKey,
                         QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const QStringList parts = text.trimmed().split(QLatin1Char('+'), Qt::KeepEmptyParts);
    if (parts.size() < 3)
        return fail(QStringLiteral("Use at least two modifier keys and one key, e.g. Ctrl+Alt+N."));

    unsigned mods = 0;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QString p = parts.at(i).trimmed().toLower();
        unsigned bit = 0;
        if (p == QLatin1String("ctrl") || p == QLatin1String("control"))
            bit = kCtrl;
        else if (p == QLatin1String("alt"))
            bit = kAlt;
        else if (p == QLatin1String("shift"))
            bit = kShift;
        else if (p == QLatin1String("win") || p == QLatin1String("meta"))
            bit = kWin;
        else
            return fail(QStringLiteral("Unknown modifier \"%1\".").arg(parts.at(i).left(12)));
        if (mods & bit)
            return fail(QStringLiteral("A modifier is repeated."));
        mods |= bit;
    }
    if (!(mods & (kCtrl | kAlt)))
        return fail(QStringLiteral("Include Ctrl or Alt, so ordinary typing is never captured."));
    if ((mods & kWin) != 0)
        return fail(QStringLiteral("Shortcuts using the Windows key are reserved by Windows."));

    const QString key = parts.last().trimmed().toUpper();
    unsigned vk = 0;
    if (key.size() == 1 && ((key[0] >= QLatin1Char('A') && key[0] <= QLatin1Char('Z'))
                            || (key[0] >= QLatin1Char('0') && key[0] <= QLatin1Char('9')))) {
        vk = key[0].unicode();
    } else if (key == QLatin1String("SPACE")) {
        vk = 0x20;
    } else if (key.size() >= 2 && key[0] == QLatin1Char('F')) {
        bool ok = false;
        const int n = key.mid(1).toInt(&ok);
        if (!ok || n < 1 || n > 12)
            return fail(QStringLiteral("Function keys F1 to F12 are supported."));
        vk = 0x70 + static_cast<unsigned>(n - 1);
    } else {
        return fail(QStringLiteral("Use a letter, a digit, F1-F12 or Space as the last key."));
    }
    if (modifiers)
        *modifiers = mods;
    if (virtualKey)
        *virtualKey = vk;
    return true;
}

GlobalHotkey::GlobalHotkey(TaskRepository &repository, const QString &key,
                           const QString &initialShortcut, const QStringList &choices, QObject *parent)
    : QObject(parent)
    , m_repository(repository)
    , m_key(key)
    , m_choices(choices)
    , m_shortcut(initialShortcut)
    , m_id(nextHotkeyId())
{
    QCoreApplication::instance()->installNativeEventFilter(this);
}

GlobalHotkey::~GlobalHotkey()
{
    release();
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
}

bool GlobalHotkey::load()
{
    QString text;
    bool found = false;
    if (!m_repository.readMetadata(m_key, &text, &found))
        return false;
    if (found) {
        const QJsonObject o = QJsonDocument::fromJson(text.toUtf8()).object();
        const QString s = o.value(QStringLiteral("shortcut")).toString();
        if (!s.isEmpty() && parse(s, nullptr, nullptr))
            m_shortcut = s;
        m_enabled = o.value(QStringLiteral("enabled")).toBool(false);
    }
    apply();
    emit changed();
    return true;
}

bool GlobalHotkey::save()
{
    QJsonObject o;
    o.insert(QStringLiteral("enabled"), m_enabled);
    o.insert(QStringLiteral("shortcut"), m_shortcut);
    return m_repository.writeMetadata(
        m_key, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

void GlobalHotkey::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    apply();
    save();
    emit changed();
}

void GlobalHotkey::setShortcut(const QString &shortcut)
{
    if (shortcut == m_shortcut)
        return;
    QString why;
    if (!parse(shortcut, nullptr, nullptr, &why)) {
        m_error = why; // the previous shortcut stays in force
        emit changed();
        return;
    }
    m_shortcut = shortcut;
    apply();
    save();
    emit changed();
}

void GlobalHotkey::release()
{
#ifdef Q_OS_WIN
    if (m_registered)
        UnregisterHotKey(nullptr, m_id);
#endif
    m_registered = false;
}

bool GlobalHotkey::apply()
{
    release();
    m_error.clear();
    if (!m_enabled)
        return true;

    unsigned mods = 0, vk = 0;
    if (!parse(m_shortcut, &mods, &vk, &m_error))
        return false;
#ifdef Q_OS_WIN
    if (!RegisterHotKey(nullptr, m_id, mods | MOD_NOREPEAT, vk)) {
        const DWORD code = GetLastError();
        m_error = code == ERROR_HOTKEY_ALREADY_REGISTERED
                      ? QStringLiteral("%1 is already used by another program. Choose a different shortcut.")
                            .arg(m_shortcut)
                      : QStringLiteral("Windows could not register %1 (error %2).")
                            .arg(m_shortcut)
                            .arg(code);
        return false;
    }
    m_registered = true;
    return true;
#else
    m_error = QStringLiteral("Global shortcuts are only available on Windows.");
    return false;
#endif
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (!m_registered || eventType != "windows_generic_MSG" || !message)
        return false;
    const MSG *msg = static_cast<const MSG *>(message);
    if (msg->message != WM_HOTKEY || static_cast<int>(msg->wParam) != m_id)
        return false;
    // Note who had the keyboard, before our own window takes it.
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground)
        GetWindowThreadProcessId(foreground, &pid);
    m_previousWindow = (foreground && pid != GetCurrentProcessId())
                           ? reinterpret_cast<quintptr>(foreground) : 0;
    if (result)
        *result = 0;
    emit activated();
    return true;
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}

void GlobalHotkey::restoreForeground()
{
#ifdef Q_OS_WIN
    HWND previous = reinterpret_cast<HWND>(m_previousWindow);
    m_previousWindow = 0;
    if (previous && IsWindow(previous))
        SetForegroundWindow(previous);
#endif
}
