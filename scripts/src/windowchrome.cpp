#include "windowchrome.h"

#include <QGuiApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

WindowChrome::WindowChrome(QObject *parent)
    : QObject(parent)
{
}

WindowChrome::~WindowChrome()
{
    detach();
}

void WindowChrome::setWindow(QWindow *window)
{
    if (m_window == window)
        return;
    detach();
    m_window = window;
    emit windowChanged();
    attach();
}

void WindowChrome::setResizeBorder(int dip)
{
    dip = qBound(2, dip, 16);
    if (m_resizeBorder == dip)
        return;
    m_resizeBorder = dip;
    emit resizeBorderChanged();
}

void WindowChrome::setMaximizeButtonRect(const QRectF &rect)
{
    if (m_maximizeRect == rect)
        return;
    m_maximizeRect = rect;
    emit maximizeButtonRectChanged();
}

void WindowChrome::setMaximizeHovered(bool hovered)
{
    if (m_maximizeHovered == hovered)
        return;
    m_maximizeHovered = hovered;
    emit maximizeHoveredChanged();
}

void WindowChrome::setMaximizePressed(bool pressed)
{
    if (m_maximizePressed == pressed)
        return;
    m_maximizePressed = pressed;
    emit maximizePressedChanged();
}

#ifdef Q_OS_WIN

namespace {
HWND hwndOf(const QWindow *window)
{
    // winId() creates the native window when there is none, so a window that
    // was already closed and destroyed would be rebuilt by every message the
    // filter looks at (an endless loop at shutdown). Only ask a live one.
    return window && window->handle() ? reinterpret_cast<HWND>(window->winId()) : nullptr;
}
} // namespace

void WindowChrome::attach()
{
    // Only the real Windows platform has an HWND behind winId() (the offscreen
    // platform used by the automated tests does not).
    if (!m_window || QGuiApplication::platformName() != QLatin1String("windows"))
        return;
    m_window->create(); // the frame is changed before the first show
    HWND hwnd = hwndOf(m_window);
    if (!hwnd)
        return;

    if (!m_filterInstalled) {
        qApp->installNativeEventFilter(this);
        m_filterInstalled = true;
    }

    applyFrame();
    // Qt may finish creating the native window after this point; apply again
    // once it is shown so the system frame is certainly gone.
    connect(m_window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (visible)
            applyFrame();
    });

    m_active = true;
    emit activeChanged();
    refreshMaximized();
}

void WindowChrome::applyFrame()
{
    HWND hwnd = hwndOf(m_window);
    if (!hwnd)
        return;
    // A one-pixel margin keeps the DWM shadow and rounded corners on a window
    // whose non-client area has been removed.
    const MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void WindowChrome::detach()
{
    if (m_window)
        disconnect(m_window, nullptr, this, nullptr);
    if (m_filterInstalled) {
        qApp->removeNativeEventFilter(this);
        m_filterInstalled = false;
    }
    if (m_active) {
        m_active = false;
        emit activeChanged();
    }
    setMaximizeHovered(false);
    setMaximizePressed(false);
}

void WindowChrome::refreshMaximized()
{
    HWND hwnd = hwndOf(m_window);
    const bool zoomed = hwnd && IsZoomed(hwnd);
    if (zoomed == m_maximized)
        return;
    m_maximized = zoomed;
    emit maximizedChanged();
}

void WindowChrome::toggleMaximized()
{
    HWND hwnd = hwndOf(m_window);
    if (!hwnd)
        return;
    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

bool WindowChrome::cursorOverMaximize(qintptr lParam) const
{
    HWND hwnd = hwndOf(m_window);
    if (!hwnd || m_maximizeRect.isEmpty())
        return false;
    RECT wr;
    if (!GetWindowRect(hwnd, &wr))
        return false;
    const qreal dpr = m_window->devicePixelRatio();
    const qreal x = (GET_X_LPARAM(lParam) - wr.left) / dpr;
    const qreal y = (GET_Y_LPARAM(lParam) - wr.top) / dpr;
    return m_maximizeRect.contains(x, y);
}

bool WindowChrome::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    if (!m_active || !m_window || eventType != "windows_generic_MSG")
        return false;
    // Messages taken from the queue (the non-client mouse buttons) are offered
    // to the filter before they are dispatched, with no result pointer.
    qintptr unused = 0;
    if (!result)
        result = &unused;
    MSG *msg = static_cast<MSG *>(message);
    if (msg->hwnd != hwndOf(m_window))
        return false;

    switch (msg->message) {
    case WM_NCCALCSIZE: {
        // The client area is the whole window. A maximized window would
        // otherwise overhang the screen by the (invisible) frame width, so it
        // is fitted to the monitor's work area instead.
        if (msg->wParam && IsZoomed(msg->hwnd)) {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
            MONITORINFO info = {};
            info.cbSize = sizeof(info);
            HMONITOR monitor = MonitorFromRect(&params->rgrc[0], MONITOR_DEFAULTTONEAREST);
            if (monitor && GetMonitorInfo(monitor, &info))
                params->rgrc[0] = info.rcWork;
        }
        *result = 0;
        return true;
    }
    case WM_NCHITTEST: {
        RECT wr;
        if (!GetWindowRect(msg->hwnd, &wr))
            return false;
        const qreal dpr = m_window->devicePixelRatio();
        const int border = qRound(m_resizeBorder * dpr);
        const int x = GET_X_LPARAM(msg->lParam);
        const int y = GET_Y_LPARAM(msg->lParam);
        const bool fixedSize = m_window->minimumWidth() > 0 && m_window->minimumWidth() == m_window->maximumWidth()
                               && m_window->minimumHeight() == m_window->maximumHeight();

        if (!IsZoomed(msg->hwnd) && !fixedSize) {
            const bool left = x >= wr.left && x < wr.left + border;
            const bool right = x < wr.right && x >= wr.right - border;
            const bool top = y >= wr.top && y < wr.top + border;
            const bool bottom = y < wr.bottom && y >= wr.bottom - border;
            LRESULT hit = 0;
            if (top && left) hit = HTTOPLEFT;
            else if (top && right) hit = HTTOPRIGHT;
            else if (bottom && left) hit = HTBOTTOMLEFT;
            else if (bottom && right) hit = HTBOTTOMRIGHT;
            else if (left) hit = HTLEFT;
            else if (right) hit = HTRIGHT;
            else if (top) hit = HTTOP;
            else if (bottom) hit = HTBOTTOM;
            if (hit) {
                *result = hit;
                return true;
            }
        }
        // Telling Windows this is the maximize button is what makes the
        // Windows 11 snap-layout flyout appear on hover.
        if (cursorOverMaximize(msg->lParam)) {
            *result = HTMAXBUTTON;
            return true;
        }
        return false;
    }
    case WM_NCMOUSEMOVE:
        if (msg->wParam == HTMAXBUTTON) {
            setMaximizeHovered(true);
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT, msg->hwnd, 0};
            TrackMouseEvent(&track);
        } else {
            setMaximizeHovered(false);
        }
        return false; // Windows still shows the flyout
    case WM_NCMOUSELEAVE:
        setMaximizeHovered(false);
        setMaximizePressed(false);
        return false;
    case WM_NCLBUTTONDOWN:
        if (msg->wParam == HTMAXBUTTON) {
            setMaximizePressed(true);
            *result = 0;
            return true;
        }
        return false;
    case WM_NCLBUTTONUP:
        if (msg->wParam == HTMAXBUTTON) {
            // The release itself landing on the button is the click; the pressed
            // flag is only the visual state and a leave notification can clear it.
            setMaximizePressed(false);
            toggleMaximized();
            *result = 0;
            return true;
        }
        return false;
    case WM_SIZE:
        refreshMaximized();
        return false;
    default:
        return false;
    }
}

#else // !Q_OS_WIN

void WindowChrome::attach() {}
void WindowChrome::applyFrame() {}
void WindowChrome::detach() {}
void WindowChrome::refreshMaximized() {}
void WindowChrome::toggleMaximized() {}
bool WindowChrome::cursorOverMaximize(qintptr) const { return false; }
bool WindowChrome::nativeEventFilter(const QByteArray &, void *, qintptr *) { return false; }

#endif
