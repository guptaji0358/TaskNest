#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// One-shot capture of the text selected in another Windows application.
//
// It runs only when asked (the user pressed the capture shortcut). It uses
// Microsoft UI Automation: the focused element of the foreground window is
// asked for its text selection through the TextPattern. Applications that do
// not expose that (many games, some custom-drawn editors, some browsers
// unless accessibility is active, elevated windows) cannot be read; the result
// then says so and the capture window offers manual paste instead.
//
// Deliberately NOT done: no keyboard hook, no synthetic Ctrl+C, no clipboard
// reads or writes, no polling, no reading of password fields, no elevation.
struct SelectionResult
{
    enum Status { Ok, Empty, Password, Unsupported, Failed };

    Status status = Failed;
    QString text;            // sanitized selection (possibly truncated, see `truncated`)
    bool truncated = false;
    QString sourceTitle;     // title of the foreground window
    QString sourceProcess;   // executable file name, e.g. "notepad.exe"
    QString message;         // short explanation for anything but Ok

    // "Window title (app.exe)" for the task notes, or an empty string.
    QString sourceText() const;
    QVariantMap toVariantMap() const;
};

class SelectionCapture : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxChars = 10000;

    explicit SelectionCapture(QObject *parent = nullptr);

    // Reads the selection of the current foreground application on a worker
    // thread (an unresponsive application cannot block TaskNest's UI) and
    // emits finished(). A call while one is running is ignored.
    Q_INVOKABLE void capture();

    // Blocking version, used by the worker.
    static SelectionResult captureNow();

    // Normalization applied to captured text: CRLF -> LF, control characters
    // removed, surrounding whitespace trimmed.
    static QString sanitize(const QString &text);

signals:
    void finished(const QVariantMap &result);

private:
    bool m_busy = false;
};
