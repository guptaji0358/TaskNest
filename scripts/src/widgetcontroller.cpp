#include "widgetcontroller.h"

#include "tasklistmodel.h"
#include "taskrepository.h"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>

#include <algorithm>

namespace {

constexpr int kSaveDelayMs = 400;
// The title strip that must stay reachable so the window can always be dragged.
constexpr int kGrabStripHeight = 36;
constexpr int kMinVisibleWidth = 80;
constexpr int kMaxSize = 4000;

int clampInt(int value, int low, int high)
{
    return std::max(low, std::min(value, high));
}

} // namespace

const QString &WidgetController::metadataKey()
{
    static const QString key = QStringLiteral("widget_settings");
    return key;
}

WidgetController::WidgetController(TaskListModel &mainModel, TaskRepository &repository,
                                   QObject *parent)
    : QObject(parent)
    , m_repository(repository)
    , m_today(new TaskListModel(repository, this))
    , m_upcoming(new TaskListModel(repository, this))
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &WidgetController::flush);

    // The two views are read-only: all changes go through the main model, whose
    // signal is the only thing that refreshes them.
    connect(&mainModel, &TaskListModel::tasksMutated, this, &WidgetController::refreshViews);
    connect(m_today, &TaskListModel::errorOccurred, this, &WidgetController::errorOccurred);
    connect(m_upcoming, &TaskListModel::errorOccurred, this, &WidgetController::errorOccurred);
}

WidgetController::~WidgetController()
{
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        flush();
    }
}

QObject *WidgetController::todayModel() const
{
    return m_today;
}

QObject *WidgetController::upcomingModel() const
{
    return m_upcoming;
}

void WidgetController::refreshViews()
{
    // First call sets the view; later calls reload it (the day may have changed).
    if (m_today->scope() != TaskListModel::Today)
        m_today->setView(TaskListModel::Today, QString(), TaskListModel::Active);
    else
        m_today->refresh();
    if (m_upcoming->scope() != TaskListModel::Upcoming)
        m_upcoming->setView(TaskListModel::Upcoming, QString(), TaskListModel::Active);
    else
        m_upcoming->refresh();
}

bool WidgetController::load()
{
    QString text;
    bool found = false;
    if (!m_repository.readMetadata(metadataKey(), &text, &found)) {
        emit errorOccurred(m_repository.lastError());
        return false;
    }
    if (!found)
        return true; // first run: defaults

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return true; // unreadable: defaults (the stored text is left as it is)
    const QJsonObject o = doc.object();

    auto boolOf = [&o](const char *key, bool fallback) {
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isBool() ? v.toBool() : fallback;
    };
    auto intOf = [&o](const char *key, int fallback) {
        const QJsonValue v = o.value(QLatin1String(key));
        return v.isDouble() ? static_cast<int>(v.toDouble()) : fallback;
    };

    // Startup visibility comes only from "showOnLaunch"; the saved "visible"
    // (where the widget was when the app last closed) is not restored.
    m_showOnLaunch = boolOf("showOnLaunch", false);
    m_reduceMotion = boolOf("reduceMotion", false);
    m_visible = m_showOnLaunch;
    m_hasPosition = o.value(QLatin1String("x")).isDouble() && o.value(QLatin1String("y")).isDouble();
    m_x = m_hasPosition ? intOf("x", 0) : 0;
    m_y = m_hasPosition ? intOf("y", 0) : 0;
    m_width = clampInt(intOf("width", kDefaultWidth), kMinWidth, kMaxSize);
    m_height = clampInt(intOf("height", kDefaultHeight), kMinHeight, kMaxSize);
    m_compact = boolOf("compact", m_compact);
    m_surface = clampInt(intOf("surface", 0), 0, kSurfaceCount - 1);
    const QJsonValue opacity = o.value(QLatin1String("opacity"));
    m_opacity = opacity.isDouble() ? std::clamp(opacity.toDouble(), kMinOpacity, 1.0) : m_opacity;
    m_alwaysOnTop = boolOf("alwaysOnTop", m_alwaysOnTop);
    m_showToday = boolOf("showToday", m_showToday);
    m_showUpcoming = boolOf("showUpcoming", m_showUpcoming);
    m_showQuickAdd = boolOf("showQuickAdd", m_showQuickAdd);
    m_showReminders = boolOf("showReminders", m_showReminders);
    m_showDetails = boolOf("showDetails", m_showDetails);

    emit widgetVisibleChanged();
    emit geometryChanged();
    emit preferencesChanged();
    return true;
}

bool WidgetController::flush()
{
    m_saveTimer.stop();
    QJsonObject o;
    o.insert(QStringLiteral("visible"), m_visible);
    o.insert(QStringLiteral("showOnLaunch"), m_showOnLaunch);
    o.insert(QStringLiteral("reduceMotion"), m_reduceMotion);
    if (m_hasPosition) {
        o.insert(QStringLiteral("x"), m_x);
        o.insert(QStringLiteral("y"), m_y);
    }
    o.insert(QStringLiteral("width"), m_width);
    o.insert(QStringLiteral("height"), m_height);
    o.insert(QStringLiteral("compact"), m_compact);
    o.insert(QStringLiteral("surface"), m_surface);
    o.insert(QStringLiteral("opacity"), m_opacity);
    o.insert(QStringLiteral("alwaysOnTop"), m_alwaysOnTop);
    o.insert(QStringLiteral("showToday"), m_showToday);
    o.insert(QStringLiteral("showUpcoming"), m_showUpcoming);
    o.insert(QStringLiteral("showQuickAdd"), m_showQuickAdd);
    o.insert(QStringLiteral("showReminders"), m_showReminders);
    o.insert(QStringLiteral("showDetails"), m_showDetails);

    const QString text = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    if (!m_repository.writeMetadata(metadataKey(), text)) {
        emit errorOccurred(m_repository.lastError());
        return false;
    }
    return true;
}

void WidgetController::scheduleSave()
{
    m_saveTimer.start(); // restarts the single-shot delay
}

bool WidgetController::recoverPosition(const QRect &saved, const QList<QRect> &screens,
                                       QPoint *result)
{
    if (screens.isEmpty() || !result)
        return false;

    // Enough of the title strip on some screen: keep the position as it is.
    const QRect strip(saved.x(), saved.y(), saved.width(), kGrabStripHeight);
    for (const QRect &screen : screens) {
        const QRect visible = strip.intersected(screen);
        if (visible.width() >= std::min(kMinVisibleWidth, saved.width()) && visible.height() >= 24) {
            *result = saved.topLeft();
            return true;
        }
    }

    // Otherwise bring it fully onto the screen it overlaps most (else the first).
    const QRect *best = &screens.first();
    qint64 bestArea = -1;
    for (const QRect &screen : screens) {
        const QRect common = saved.intersected(screen);
        const qint64 area = common.isValid() ? static_cast<qint64>(common.width()) * common.height() : 0;
        if (area > bestArea) {
            bestArea = area;
            best = &screen;
        }
    }
    const int maxX = std::max(best->left(), best->right() + 1 - saved.width());
    const int maxY = std::max(best->top(), best->bottom() + 1 - saved.height());
    *result = QPoint(clampInt(saved.x(), best->left(), maxX), clampInt(saved.y(), best->top(), maxY));
    return true;
}

QVariantMap WidgetController::recoveredPosition(int width, int height) const
{
    QList<QRect> screens;
    if (QScreen *primary = QGuiApplication::primaryScreen())
        screens << primary->availableGeometry();
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen != QGuiApplication::primaryScreen())
            screens << screen->availableGeometry();
    }

    QPoint point;
    QRect wanted(m_x, m_y, width, height);
    if (!m_hasPosition && !screens.isEmpty()) {
        // First use: top-right corner of the primary screen.
        const QRect area = screens.first();
        wanted.moveTo(area.right() + 1 - width - 24, area.top() + 24);
    }
    QVariantMap map;
    if (!recoverPosition(wanted, screens, &point)) {
        map.insert(QStringLiteral("valid"), false);
        return map;
    }
    map.insert(QStringLiteral("valid"), true);
    map.insert(QStringLiteral("x"), point.x());
    map.insert(QStringLiteral("y"), point.y());
    return map;
}

template <typename T>
void WidgetController::assign(T &field, const T &value, void (WidgetController::*signal)())
{
    if (field == value)
        return;
    field = value;
    emit (this->*signal)();
    scheduleSave();
}

void WidgetController::prepareForShutdown()
{
    flush();
    m_shuttingDown = true;
}

void WidgetController::setWidgetVisible(bool visible)
{
    if (m_shuttingDown)
        return;
    assign(m_visible, visible, &WidgetController::widgetVisibleChanged);
}

void WidgetController::setX(int x)
{
    if (m_hasPosition && x == m_x)
        return;
    m_hasPosition = true;
    m_x = x;
    emit geometryChanged();
    scheduleSave();
}

void WidgetController::setY(int y)
{
    if (m_hasPosition && y == m_y)
        return;
    m_hasPosition = true;
    m_y = y;
    emit geometryChanged();
    scheduleSave();
}

void WidgetController::setWidth(int width)
{
    assign(m_width, clampInt(width, kMinWidth, kMaxSize), &WidgetController::geometryChanged);
}

void WidgetController::setHeight(int height)
{
    assign(m_height, clampInt(height, kMinHeight, kMaxSize), &WidgetController::geometryChanged);
}

void WidgetController::setShowOnLaunch(bool show)
{
    assign(m_showOnLaunch, show, &WidgetController::preferencesChanged);
}

void WidgetController::setReduceMotion(bool reduce)
{
    assign(m_reduceMotion, reduce, &WidgetController::preferencesChanged);
}

void WidgetController::setCompact(bool compact)
{
    assign(m_compact, compact, &WidgetController::preferencesChanged);
}

void WidgetController::setSurface(int surface)
{
    assign(m_surface, clampInt(surface, 0, kSurfaceCount - 1), &WidgetController::preferencesChanged);
}

void WidgetController::setOpacity(double opacity)
{
    assign(m_opacity, std::clamp(opacity, kMinOpacity, 1.0), &WidgetController::preferencesChanged);
}

void WidgetController::setAlwaysOnTop(bool onTop)
{
    assign(m_alwaysOnTop, onTop, &WidgetController::preferencesChanged);
}

void WidgetController::setShowToday(bool show)
{
    assign(m_showToday, show, &WidgetController::preferencesChanged);
}

void WidgetController::setShowUpcoming(bool show)
{
    assign(m_showUpcoming, show, &WidgetController::preferencesChanged);
}

void WidgetController::setShowQuickAdd(bool show)
{
    assign(m_showQuickAdd, show, &WidgetController::preferencesChanged);
}

void WidgetController::setShowReminders(bool show)
{
    assign(m_showReminders, show, &WidgetController::preferencesChanged);
}

void WidgetController::setShowDetails(bool show)
{
    assign(m_showDetails, show, &WidgetController::preferencesChanged);
}

void WidgetController::resetPreferences()
{
    setCompact(false);
    setSurface(0);
    setOpacity(0.95);
    setAlwaysOnTop(true);
    setShowToday(true);
    setShowUpcoming(true);
    setShowQuickAdd(true);
    setShowReminders(true);
    setShowDetails(true);
}
