#pragma once

#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Read-only / delete helpers for HKEY_CURRENT_USER keys named like
// "HKEY_CURRENT_USER\\Software\\Classes\\tasknest".
//
// QSettings creates the key it is pointed at as soon as it is constructed on
// Windows, so merely *asking* whether TaskNest is registered would leave an
// empty key behind. These helpers open keys for reading only and never create
// anything. Only the HKEY_CURRENT_USER root is accepted.
namespace RegistryUtil {

inline QString subPathOf(const QString &fullKey)
{
    static const QString root = QStringLiteral("HKEY_CURRENT_USER\\");
    if (!fullKey.startsWith(root, Qt::CaseInsensitive))
        return QString();
    return fullKey.mid(root.size());
}

// True if the key exists; when `value` is not null and the key's default
// (unnamed) value is a string, stores it.
inline bool readDefault(const QString &fullKey, QString *value = nullptr)
{
#ifdef Q_OS_WIN
    const QString sub = subPathOf(fullKey);
    if (sub.isEmpty())
        return false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(sub.utf16()), 0, KEY_READ, &key)
        != ERROR_SUCCESS)
        return false;
    if (value) {
        value->clear();
        wchar_t buffer[2048];
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        if (RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size)
                == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            *value = QString::fromWCharArray(buffer, static_cast<int>(size / sizeof(wchar_t)));
            while (value->endsWith(QChar(0)))
                value->chop(1);
        }
    }
    RegCloseKey(key);
    return true;
#else
    Q_UNUSED(fullKey);
    Q_UNUSED(value);
    return false;
#endif
}

inline bool keyExists(const QString &fullKey)
{
    return readDefault(fullKey, nullptr);
}

// Removes the key and everything below it. A key that is not there is fine.
inline bool deleteTree(const QString &fullKey)
{
#ifdef Q_OS_WIN
    const QString sub = subPathOf(fullKey);
    if (sub.isEmpty())
        return false;
    const LONG r = RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(sub.utf16()));
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
#else
    Q_UNUSED(fullKey);
    return true;
#endif
}

} // namespace RegistryUtil
