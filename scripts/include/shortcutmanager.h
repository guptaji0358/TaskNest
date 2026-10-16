#pragma once

#include <QObject>
#include <QString>

#include "testprofile.h"

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
    // Whether Windows is set to open tasknest:// links with this executable.
    Q_PROPERTY(bool linksRegistered READ linksRegistered NOTIFY changed)
    // Whether Chrome/Edge are set up to reach TaskNest's native host.
    Q_PROPERTY(bool browserHostRegistered READ browserHostRegistered NOTIFY changed)

public:
    explicit ShortcutManager(QObject *parent = nullptr);

    bool supported() const;
    bool startMenuShortcut() const;
    bool taskbarPinned() const;
    bool linksRegistered() const;
    bool browserHostRegistered() const;

    // Creates (replacing a stale one) or removes the shortcut. False on
    // failure; the reason is in lastError().
    Q_INVOKABLE bool setStartMenuShortcut(bool enabled);
    // Cannot pin silently: ensures the shortcut and reveals it. Returns the
    // text to show the user (empty on failure; see lastError()).
    Q_INVOKABLE QString requestTaskbarPin();
    // Registers / removes the tasknest:// handler under HKEY_CURRENT_USER.
    Q_INVOKABLE bool setLinksRegistered(bool enabled);
    // Registers / removes the Chrome and Edge native-messaging host
    // (HKEY_CURRENT_USER, for the TaskNest extension id only).
    Q_INVOKABLE bool setBrowserHostRegistered(bool enabled);
    // Re-reads the state (the user may have pinned or unpinned in Windows).
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString lastError() const { return m_error; }

    // Scratch test profile (--test-profile): browser host registration goes to
    // the profile's own registry subtree and folder; per-user shortcuts and the
    // tasknest:// scheme, which have no scratch equivalent, are refused.
    void setTestProfile(const TestProfile::Profile &profile) { m_profile = profile; }

    // Overridable for tests.
    static QString startMenuShortcutPath();
    static QString pinnedTaskbarFolder();

signals:
    void changed();

private:
    QString m_error;
    TestProfile::Profile m_profile;
};
