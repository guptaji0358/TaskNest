#include "selectioncapture.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#endif

QString SelectionResult::sourceText() const
{
    const QString title = sourceTitle.simplified();
    if (title.isEmpty() && sourceProcess.isEmpty())
        return QString();
    if (sourceProcess.isEmpty())
        return title;
    if (title.isEmpty())
        return sourceProcess;
    return QStringLiteral("%1 (%2)").arg(title.left(200), sourceProcess);
}

QVariantMap SelectionResult::toVariantMap() const
{
    static const char *names[] = {"ok", "empty", "password", "unsupported", "failed"};
    QVariantMap m;
    m.insert(QStringLiteral("status"), QString::fromLatin1(names[status]));
    m.insert(QStringLiteral("text"), text);
    m.insert(QStringLiteral("truncated"), truncated);
    m.insert(QStringLiteral("source"), sourceText());
    m.insert(QStringLiteral("message"), message);
    return m;
}

QString SelectionCapture::sanitize(const QString &input)
{
    QString text = input;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if ((u < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\t')) || u == 0x7f)
            continue;
        out += c;
    }
    return out.trimmed();
}

SelectionCapture::SelectionCapture(QObject *parent)
    : QObject(parent)
{
}

void SelectionCapture::capture()
{
    if (m_busy)
        return;
    m_busy = true;
    QPointer<SelectionCapture> self(this);
    QThread *thread = QThread::create([self]() {
        const SelectionResult result = SelectionCapture::captureNow();
        if (self) {
            QMetaObject::invokeMethod(self.data(), [self, result]() {
                if (!self)
                    return;
                self->m_busy = false;
                emit self->finished(result.toVariantMap());
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

#ifdef Q_OS_WIN

namespace {

template <typename T>
void release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

QString processNameOf(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return QString();
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    QString name;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size))
        name = QFileInfo(QString::fromWCharArray(buffer, static_cast<int>(size))).fileName();
    CloseHandle(process);
    return name;
}

} // namespace

SelectionResult SelectionCapture::captureNow()
{
    SelectionResult result;

    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &pid);
        wchar_t title[256];
        const int n = GetWindowTextW(foreground, title, 256);
        result.sourceTitle = QString::fromWCharArray(title, n);
        result.sourceProcess = processNameOf(pid);
    }
    if (!foreground || pid == 0) {
        result.status = SelectionResult::Failed;
        result.message = QStringLiteral("No application window had the keyboard.");
        return result;
    }
    if (pid == GetCurrentProcessId()) {
        result.status = SelectionResult::Unsupported;
        result.message = QStringLiteral("Select text in another application first.");
        return result;
    }

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);

    IUIAutomation *automation = nullptr;
    IUIAutomationElement *element = nullptr;
    IUnknown *patternUnknown = nullptr;
    IUIAutomationTextPattern *textPattern = nullptr;
    IUIAutomationTextRangeArray *ranges = nullptr;

    auto finish = [&]() {
        release(ranges);
        release(textPattern);
        release(patternUnknown);
        release(element);
        release(automation);
        if (uninit)
            CoUninitialize();
        return result;
    };

    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IUIAutomation), reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || !automation) {
        result.status = SelectionResult::Failed;
        result.message = QStringLiteral("Windows UI Automation is not available.");
        return finish();
    }

    hr = automation->GetFocusedElement(&element);
    if (FAILED(hr) || !element) {
        result.status = SelectionResult::Unsupported;
        result.message = QStringLiteral("This application does not share what is selected.");
        return finish();
    }

    // Never read password fields.
    VARIANT isPassword;
    VariantInit(&isPassword);
    if (SUCCEEDED(element->GetCurrentPropertyValue(UIA_IsPasswordPropertyId, &isPassword))
        && isPassword.vt == VT_BOOL && isPassword.boolVal != VARIANT_FALSE) {
        VariantClear(&isPassword);
        result.status = SelectionResult::Password;
        result.message = QStringLiteral("A password field is focused; nothing was read.");
        return finish();
    }
    VariantClear(&isPassword);

    int elementPid = 0;
    if (SUCCEEDED(element->get_CurrentProcessId(&elementPid)) && elementPid == static_cast<int>(GetCurrentProcessId())) {
        result.status = SelectionResult::Unsupported;
        result.message = QStringLiteral("Select text in another application first.");
        return finish();
    }

    hr = element->GetCurrentPattern(UIA_TextPatternId, &patternUnknown);
    if (FAILED(hr) || !patternUnknown) {
        result.status = SelectionResult::Unsupported;
        result.message = QStringLiteral("This application does not share what is selected.");
        return finish();
    }
    hr = patternUnknown->QueryInterface(__uuidof(IUIAutomationTextPattern),
                                        reinterpret_cast<void **>(&textPattern));
    if (FAILED(hr) || !textPattern) {
        result.status = SelectionResult::Unsupported;
        result.message = QStringLiteral("This application does not share what is selected.");
        return finish();
    }

    hr = textPattern->GetSelection(&ranges);
    if (FAILED(hr) || !ranges) {
        result.status = SelectionResult::Unsupported;
        result.message = QStringLiteral("This application does not share what is selected.");
        return finish();
    }

    int length = 0;
    ranges->get_Length(&length);
    QString text;
    for (int i = 0; i < length && i < 8 && text.size() <= kMaxChars; ++i) {
        IUIAutomationTextRange *range = nullptr;
        if (FAILED(ranges->GetElement(i, &range)) || !range)
            continue;
        BSTR chunk = nullptr;
        if (SUCCEEDED(range->GetText(kMaxChars + 1, &chunk)) && chunk) {
            if (!text.isEmpty())
                text += QLatin1Char('\n');
            text += QString::fromWCharArray(chunk, static_cast<int>(SysStringLen(chunk)));
            SysFreeString(chunk);
        }
        range->Release();
    }

    text = sanitize(text);
    if (text.isEmpty()) {
        result.status = SelectionResult::Empty;
        result.message = QStringLiteral("Nothing is selected.");
        return finish();
    }
    if (text.size() > kMaxChars) {
        text = text.left(kMaxChars);
        result.truncated = true;
        result.message = QStringLiteral("The selection is longer than %1 characters; only the "
                                        "first %1 are shown.").arg(kMaxChars);
    }
    result.status = SelectionResult::Ok;
    result.text = text;
    return finish();
}

#else

SelectionResult SelectionCapture::captureNow()
{
    SelectionResult r;
    r.status = SelectionResult::Unsupported;
    r.message = QStringLiteral("Selection capture is only available on Windows.");
    return r;
}

#endif
