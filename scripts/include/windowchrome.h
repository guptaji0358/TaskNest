#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QWindow>

// Gives a top-level QML window a custom title bar while keeping everything
// Windows does for a normal window: Aero Snap, the Windows 11 snap-layout
// flyout over the maximize button, resize borders, the system shadow and
// rounded corners, and minimize/maximize animations.
//
// The window keeps its standard style bits; only the non-client area is
// removed (WM_NCCALCSIZE), so the client area is the whole window. The QML side
// draws the title bar and the three caption buttons, drags with
// QWindow::startSystemMove and reports where the maximize button is so Windows
// can treat it as HTMAXBUTTON. On other platforms the class does nothing and
// `active` stays false, so QML falls back to the system frame.
class WindowChrome : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    Q_PROPERTY(QWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(int resizeBorder READ resizeBorder WRITE setResizeBorder NOTIFY resizeBorderChanged)
    Q_PROPERTY(QRectF maximizeButtonRect READ maximizeButtonRect WRITE setMaximizeButtonRect
                   NOTIFY maximizeButtonRectChanged)
    Q_PROPERTY(bool maximizeHovered READ maximizeHovered NOTIFY maximizeHoveredChanged)
    Q_PROPERTY(bool maximizePressed READ maximizePressed NOTIFY maximizePressedChanged)
    Q_PROPERTY(bool maximized READ isMaximized NOTIFY maximizedChanged)

public:
    explicit WindowChrome(QObject *parent = nullptr);
    ~WindowChrome() override;

    QWindow *window() const { return m_window; }
    void setWindow(QWindow *window);

    bool isActive() const { return m_active; }

    int resizeBorder() const { return m_resizeBorder; }
    void setResizeBorder(int dip);

    QRectF maximizeButtonRect() const { return m_maximizeRect; }
    void setMaximizeButtonRect(const QRectF &rect);

    bool maximizeHovered() const { return m_maximizeHovered; }
    bool maximizePressed() const { return m_maximizePressed; }
    bool isMaximized() const { return m_maximized; }

    // Maximizes the window, or restores it when it already is.
    Q_INVOKABLE void toggleMaximized();

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void windowChanged();
    void activeChanged();
    void resizeBorderChanged();
    void maximizeButtonRectChanged();
    void maximizeHoveredChanged();
    void maximizePressedChanged();
    void maximizedChanged();

private:
    void attach();
    void applyFrame();
    void detach();
    void refreshMaximized();
    void setMaximizeHovered(bool hovered);
    void setMaximizePressed(bool pressed);
    bool cursorOverMaximize(qintptr lParam) const;

    QPointer<QWindow> m_window;
    quintptr m_hwnd = 0;
    bool m_active = false;
    bool m_maximized = false;
    int m_resizeBorder = 6;
    QRectF m_maximizeRect;
    bool m_maximizeHovered = false;
    bool m_maximizePressed = false;
    bool m_filterInstalled = false;
};
