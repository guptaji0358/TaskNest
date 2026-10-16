#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <QStringList>

class TaskRepository;

// Optional system-wide shortcut that opens the compact Quick Add window.
//
// Registered with the Windows RegisterHotKey API: the system delivers one
// message when exactly that key combination is pressed. There is no keyboard
// hook, no key logging and no clipboard access. The registration is released
// when the shortcut is disabled and when the application exits, and exists
// only while TaskNest is running.
//
// The choice is stored in app_metadata ("global_quick_add" as JSON) and is OFF
// until the user turns it on in Settings.
class GlobalHotkey : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(QString shortcut READ shortcut WRITE setShortcut NOTIFY changed)
    // True only while Windows really holds the registration for this process.
    Q_PROPERTY(bool registered READ isRegistered NOTIFY changed)
    // Why the registration is not active ("" when it is or when disabled).
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
    Q_PROPERTY(QStringList choices READ choices CONSTANT)

public:
    static const QString &metadataKey();   // the Quick Add shortcut's key
    static QString defaultShortcut() { return QStringLiteral("Ctrl+Alt+N"); }
    static QStringList defaultChoices();

    // Modifier bits (MOD_*) and virtual-key code for a combination such as
    // "Ctrl+Alt+N". Requires two modifiers, at least one of them Ctrl or Alt,
    // and a letter, digit, F1-F12 or Space.
    static bool parse(const QString &text, unsigned *modifiers, unsigned *virtualKey,
                      QString *error = nullptr);

    // `key` is the app_metadata entry holding this shortcut's choice, so more
    // than one independent shortcut can exist (Quick Add, capture selection).
    explicit GlobalHotkey(TaskRepository &repository, const QString &key = metadataKey(),
                          const QString &initialShortcut = GlobalHotkey::defaultShortcut(),
                          const QStringList &choices = defaultChoices(), QObject *parent = nullptr);
    ~GlobalHotkey() override;

    // Reads the saved choice and registers it when enabled.
    bool load();

    QStringList choices() const { return m_choices; }
    bool enabled() const { return m_enabled; }
    QString shortcut() const { return m_shortcut; }
    bool isRegistered() const { return m_registered; }
    QString lastError() const { return m_error; }

    void setEnabled(bool enabled);
    void setShortcut(const QString &shortcut);

    // Windows: remembers the window that had the keyboard when the shortcut
    // was pressed, and gives it back when Quick Add closes.
    Q_INVOKABLE void restoreForeground();

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void changed();
    void activated();

private:
    bool apply();      // registers according to m_enabled / m_shortcut
    void release();
    bool save();

    TaskRepository &m_repository;
    QString m_key;
    QStringList m_choices;
    bool m_enabled = false;
    QString m_shortcut;
    bool m_registered = false;
    QString m_error;
    int m_id = 0;
    quintptr m_previousWindow = 0;
};
