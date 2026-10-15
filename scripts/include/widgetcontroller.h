#pragma once

#include <QList>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVariantMap>

class TaskListModel;
class TaskRepository;

// Backing object for the floating desktop widget (a second top-level window).
//
//  * Task data: the widget shows two read-only TaskListModel views (today,
//    upcoming) over the same repository. Every change - made in the widget or
//    in the main window - goes through the main TaskListModel; its
//    tasksMutated() signal refreshes the views. There is no timer or polling.
//  * Preferences (visibility, geometry, sections, appearance) are stored as
//    one JSON value in app_metadata ("widget_settings"). Changes are written
//    after a short single-shot delay, so dragging does not hammer the disk;
//    flush() writes at once. A missing or unreadable value means the defaults;
//    individual values are clamped to their allowed range.
class WidgetController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *todayModel READ todayModel CONSTANT)
    Q_PROPERTY(QObject *upcomingModel READ upcomingModel CONSTANT)

    Q_PROPERTY(bool widgetVisible READ widgetVisible WRITE setWidgetVisible NOTIFY widgetVisibleChanged)
    Q_PROPERTY(int x READ x WRITE setX NOTIFY geometryChanged)
    Q_PROPERTY(int y READ y WRITE setY NOTIFY geometryChanged)
    Q_PROPERTY(int width READ width WRITE setWidth NOTIFY geometryChanged)
    Q_PROPERTY(int height READ height WRITE setHeight NOTIFY geometryChanged)
    Q_PROPERTY(bool hasPosition READ hasPosition NOTIFY geometryChanged)

    Q_PROPERTY(bool compact READ compact WRITE setCompact NOTIFY preferencesChanged)
    Q_PROPERTY(int surface READ surface WRITE setSurface NOTIFY preferencesChanged)
    Q_PROPERTY(double opacity READ opacity WRITE setOpacity NOTIFY preferencesChanged)
    Q_PROPERTY(bool alwaysOnTop READ alwaysOnTop WRITE setAlwaysOnTop NOTIFY preferencesChanged)
    Q_PROPERTY(bool showToday READ showToday WRITE setShowToday NOTIFY preferencesChanged)
    Q_PROPERTY(bool showUpcoming READ showUpcoming WRITE setShowUpcoming NOTIFY preferencesChanged)
    Q_PROPERTY(bool showQuickAdd READ showQuickAdd WRITE setShowQuickAdd NOTIFY preferencesChanged)
    Q_PROPERTY(bool showReminders READ showReminders WRITE setShowReminders NOTIFY preferencesChanged)
    Q_PROPERTY(bool showDetails READ showDetails WRITE setShowDetails NOTIFY preferencesChanged)
    // Whether the widget opens when the application starts. Off by default; the
    // only thing that decides startup visibility (the last session's state is
    // not restored).
    Q_PROPERTY(bool showOnLaunch READ showOnLaunch WRITE setShowOnLaunch NOTIFY preferencesChanged)

public:
    static constexpr int kMinWidth = 220;
    static constexpr int kMinHeight = 200;
    static constexpr int kDefaultWidth = 320;
    static constexpr int kDefaultHeight = 460;
    static constexpr double kMinOpacity = 0.6;
    static constexpr int kSurfaceCount = 3; // graphite, deep, raised
    static const QString &metadataKey();

    // `mainModel` and `repository` must outlive the controller.
    WidgetController(TaskListModel &mainModel, TaskRepository &repository,
                     QObject *parent = nullptr);
    ~WidgetController() override;

    QObject *todayModel() const;
    QObject *upcomingModel() const;

    // Reads the saved preferences; false (defaults kept) if they could not be read.
    bool load();
    // Writes the preferences now (also called by the delayed save).
    Q_INVOKABLE bool flush();
    // Called when the application is closing: writes the preferences and from
    // then on ignores visibility changes, so the windows being closed by the
    // shutdown do not overwrite "the widget was open" with "hidden".
    Q_INVOKABLE void prepareForShutdown();
    // Reloads both views from the database (app re-activated: the day may have changed).
    Q_INVOKABLE void refreshViews();

    // The saved window position moved onto a visible screen, or empty QPoint
    // data when nothing was saved (the caller then picks a default).
    // `width`/`height` is the size the window will have.
    Q_INVOKABLE QVariantMap recoveredPosition(int width, int height) const;

    // Pure and testable: where a window of `size` saved at `saved` should be
    // placed given the available screen areas. Keeps the saved position when
    // enough of the window is visible, otherwise moves it fully onto the
    // screen it overlaps most (or `screens.first()`); returns false if there
    // are no screens.
    static bool recoverPosition(const QRect &saved, const QList<QRect> &screens,
                                QPoint *result);

    bool widgetVisible() const { return m_visible; }
    int x() const { return m_x; }
    int y() const { return m_y; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    bool hasPosition() const { return m_hasPosition; }
    bool compact() const { return m_compact; }
    int surface() const { return m_surface; }
    double opacity() const { return m_opacity; }
    bool alwaysOnTop() const { return m_alwaysOnTop; }
    bool showToday() const { return m_showToday; }
    bool showUpcoming() const { return m_showUpcoming; }
    bool showQuickAdd() const { return m_showQuickAdd; }
    bool showReminders() const { return m_showReminders; }
    bool showDetails() const { return m_showDetails; }
    bool showOnLaunch() const { return m_showOnLaunch; }

    void setWidgetVisible(bool visible);
    void setX(int x);
    void setY(int y);
    void setWidth(int width);
    void setHeight(int height);
    void setCompact(bool compact);
    void setSurface(int surface);
    void setOpacity(double opacity);
    void setAlwaysOnTop(bool onTop);
    void setShowToday(bool show);
    void setShowUpcoming(bool show);
    void setShowQuickAdd(bool show);
    void setShowReminders(bool show);
    void setShowDetails(bool show);
    void setShowOnLaunch(bool show);

    // Restores every preference (not the position) to its default.
    Q_INVOKABLE void resetPreferences();

signals:
    void widgetVisibleChanged();
    void geometryChanged();
    void preferencesChanged();
    void errorOccurred(const QString &message);

private:
    void scheduleSave();
    template <typename T>
    void assign(T &field, const T &value, void (WidgetController::*signal)());

    TaskRepository &m_repository;
    TaskListModel *m_today;
    TaskListModel *m_upcoming;
    QTimer m_saveTimer;

    bool m_shuttingDown = false;
    bool m_visible = false;
    bool m_hasPosition = false;
    int m_x = 0;
    int m_y = 0;
    int m_width = kDefaultWidth;
    int m_height = kDefaultHeight;
    bool m_compact = false;
    int m_surface = 0;
    double m_opacity = 0.95;
    bool m_alwaysOnTop = true;
    bool m_showToday = true;
    bool m_showUpcoming = true;
    bool m_showQuickAdd = true;
    bool m_showReminders = true;
    bool m_showDetails = true;
    bool m_showOnLaunch = false;
};
