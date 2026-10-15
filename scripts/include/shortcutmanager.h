#pragma once

#include <QObject>
#include <QString>

// Start Menu shortcut and taskbar-pin state for the Settings dialog.
//
//  * Start Menu shortcut: a single TaskNest.lnk in the per-user Programs folder.
//    The state is the file itself (no stored flag), so it can never be
//    duplicated and always reflects what is really there.
//  * Taskbar pin: Windows offers no supported API for an app to pin itself
//    silently (the old "taskbarpin" shell verb was removed in Windows 10). The
//    state is read from the user's pinned-taskbar folder; requesting a pin
//    makes sure the Start Menu shortcut exists and shows it in Explorer, so
//    the user can use Windows' own "Pin to taskbar".
class ShortcutManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool startMenuShortcut READ startMenuShortcut NOTIFY changed)
    Q_PROPERTY(bool taskbarPinned READ taskbarPinned NOTIFY changed)
    Q_PROPERTY(bool supported READ supported CONSTANT)

public:
    explicit ShortcutManager(QObject *parent = nullptr);

    bool supported() const;
    bool startMenuShortcut() const;
    bool taskbarPinned() const;

    // Creates (replacing a stale one) or removes the shortcut. False on
    // failure; the reason is in lastError().
    Q_INVOKABLE bool setStartMenuShortcut(bool enabled);
    // Cannot pin silently: ensures the shortcut and reveals it. Returns the
    // text to show the user (empty on failure; see lastError()).
    Q_INVOKABLE QString requestTaskbarPin();
    // Re-reads the state (the user may have pinned or unpinned in Windows).
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString lastError() const { return m_error; }

    // Overridable for tests.
    static QString startMenuShortcutPath();
    static QString pinnedTaskbarFolder();

signals:
    void changed();

private:
    QString m_error;
};
