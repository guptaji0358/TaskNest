# TaskNest Capture (Chrome / Edge extension)

Sends the text you selected on a web page, together with the page address, to
your local TaskNest app. Nothing leaves your computer: there is no server, no
account and no analytics.

```
browser page ──(you click)──▶ extension ──native message──▶ TaskNestHost.exe
                                                              │ reads %LOCALAPPDATA%\TaskNest\integration\*.json
                                                              ▼
                                            TaskNest.exe  (per-user named pipe, authenticated, `capture_page`)
```

Browsers cannot open Windows named pipes, so a small companion program,
`TaskNestHost.exe` (built next to `TaskNest.exe`), bridges the two with
Chrome/Edge *Native Messaging*. The browser starts it for one message and it
exits after answering. It never starts TaskNest and it never returns the
integration credential to the browser.

## Requirements

* Windows, Chrome or Edge 110+ (Manifest V3).
* `TaskNest.exe` and `TaskNestHost.exe` built (`cmake --build out/build`).
  The browser starts the host with *its own* environment, not your shell's
  PATH. Turning on **Browser capture** therefore registers a small launcher
  (`%LOCALAPPDATA%\TaskNest\native-host\com.tasknest.host.bat`) that puts the
  Qt/MinGW folders TaskNest itself is running from in front of PATH and then
  starts `TaskNestHost.exe`. Nothing is copied or bundled; if you move the Qt
  installation, switch Browser capture off and on again.
* In TaskNest: **Settings → Browser capture (Chrome / Edge)** switched on, and
  **Settings → Local integration API** switched on. The extension never turns
  either on for you.

## Install (unpacked)

1. In TaskNest Settings turn on **Browser capture (Chrome / Edge)**. This
   writes `%LOCALAPPDATA%\TaskNest\native-host\com.tasknest.host.json` and
   registers it for Chrome and Edge under `HKEY_CURRENT_USER` (no administrator
   rights), allowing exactly this extension id:
   `chrome-extension://fghdblbielgdmgnmodfhdifeiggaglcj/`.
2. In Chrome or Edge open `chrome://extensions` / `edge://extensions`, enable
   **Developer mode**, choose **Load unpacked** and select this
   `browser-extension` folder. The id shown must be the one above (it comes from
   the `key` in `manifest.json`; if it differs, capture is refused by the
   browser).
3. In TaskNest Settings turn on **Local integration API**.

## Use

1. Select text on a web page.
2. Click the TaskNest toolbar icon (or right-click → **Capture selection to
   TaskNest**).
3. In the popup review the selection and source, edit the title if you like and
   press **Capture** (or Enter in the title field).

The context-menu route captures immediately with the default title and shows
a badge on the toolbar icon (hover for the message): **✓** saved; **!** TaskNest
definitely did not save it (not running, integration off, refused request: capture
again); **?** TaskNest did not confirm in time, so the capture **may or may not
have been saved**. A `?` is not a failure: look in TaskNest before capturing again.
The context-menu route has no "Check again": selecting and capturing again is a
new capture and can create a second task. To retry the same capture without
duplicating it, use the popup, whose **Check again** re-sends the same capture.
Both routes use the same code (`capture-core.js`).

Pages the browser does not let extensions read (`chrome://`, the web store) and
non-web addresses (`file:`, `ftp:`, …) cannot be captured. A selection inside a
text box or password field is refused. Selections longer than 10 000
characters are refused (select less). Capturing the same selection twice from
two popups creates two tasks; a *retry* of one capture (same popup) does not.

## Permissions and why

| Permission | Why it is needed |
|---|---|
| `activeTab` | Lets the extension read the selection of the page you clicked the icon on, only after that click. No standing access to any site. |
| `scripting` | `chrome.scripting.executeScript` runs one function in that tab to read `window.getSelection()`. |
| `contextMenus` | The "Capture selection to TaskNest" menu item. |
| `nativeMessaging` | Required to talk to `TaskNestHost.exe`. |

Not requested: `storage`, `tabs`, `history`, `cookies`, `webRequest`,
`notifications`, `<all_urls>` or any host permission; no content scripts are
injected. The strict default extension CSP applies (no inline script, no
remote code).

## What the extension does and does not do

* Reads page content only when you click the icon / menu item.
* Never reads form fields, cookies, headers or history.
* Sets popup text only with `textContent`/`value` (page text is never
  interpreted as HTML).
* Sends one message `{op:"capture", title, url, text, request_id}` to
  `TaskNestHost.exe`; it never sees or stores the integration token.
* Does not log captured text, titles, addresses or tokens.

## Native host registration and removal

Easiest: the **Browser capture (Chrome / Edge)** switch in TaskNest Settings
(on = register, off = remove). The same by hand (PowerShell, adjust the path):

```powershell
$manifest = "$env:LOCALAPPDATA\TaskNest\native-host\com.tasknest.host.json"
# Chrome
New-Item -Force "HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.tasknest.host" |
  Set-ItemProperty -Name "(default)" -Value $manifest
# Edge
New-Item -Force "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.tasknest.host" |
  Set-ItemProperty -Name "(default)" -Value $manifest
```

Removal / uninstall (touches no TaskNest task data):

```powershell
Remove-Item -Recurse "HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.tasknest.host"
Remove-Item -Recurse "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.tasknest.host"
Remove-Item "$env:LOCALAPPDATA\TaskNest\native-host\com.tasknest.host.json"
```

Then remove the extension from `chrome://extensions` / `edge://extensions`.

## Troubleshooting

| Message in the popup | Meaning |
|---|---|
| "TaskNest is not running" | Start TaskNest. The extension never starts it. |
| "…local integration API is off" | TaskNest is running; turn on **Settings → Local integration API**. |
| "The TaskNest browser bridge is not installed" | The browser could not start the host: turn on **Settings → Browser capture**, check that `TaskNestHost.exe` exists next to `TaskNest.exe` and that the extension id matches. |
| "TaskNest refused the request" | The integration credential changed (the API was switched off/on). Capture again. |
| "TaskNest did not answer in time" | TaskNest was busy; try again. |
| "…may or may not have been saved" | The request reached the bridge but no confirmation came back, so the task may exist. Check TaskNest, or press **Check again** in the popup: it re-sends the same request id, so the capture is never saved twice. The popup keeps the title locked meanwhile. A new popup is a new capture. |

Quick check without the browser: run
`TaskNestHost.exe` is a Native Messaging program (framed JSON on stdin/stdout),
so use the browser, or see `INTEGRATION.md` for the exact protocol.

## What has and has not been verified in a real browser

Verified in real Microsoft Edge (scratch profile, unpacked extension): the
extension loads with the id above, and its service worker sends native messages
through the launcher and `TaskNestHost.exe` to a running `TaskNest.exe`
(status, capture, retry, rejected scheme), with no Qt folder on Edge's PATH.

**Not automated, to be checked by hand** (Edge's toolbar and context menus are
native UI that could not be driven reliably, and `chrome.action.openPopup()`
does not grant `activeTab`, so the popup cannot be exercised without a real
click):

1. Select text on a web page, click the TaskNest toolbar icon: the popup shows
   the text, page title and address; press Capture once: one task appears in
   TaskNest with `Source: <address>` at the end of its notes.
2. Right-click a selection -> **Capture selection to TaskNest**: the toolbar
   icon shows a check mark badge and the task appears.
3. With no selection, on `edge://` / `chrome://` pages, with TaskNest closed and
   with the integration API off: the popup explains what to do and no task is
   created.

## Tests

* `ctest --test-dir out/build --output-on-failure` runs, among others:
  `tests/qml/tst_extension.qml` (the shared capture logic in
  `capture-core.js`), and in `tst_tasknest` the native-host framing, validation,
  registration scoped to the extension id, manifest/permission checks and the
  end-to-end capture against a real integration endpoint.
* Real-executable and real-browser checks are manual/scripted and are described
  in `INTEGRATION.md`.

## Files

| File | Purpose |
|---|---|
| `manifest.json` | MV3 manifest; `key` fixes the extension id |
| `capture-core.js` | validation, title derivation, request building, error messages (shared by popup, service worker and tests) |
| `popup.html` / `popup.css` / `popup.js` | review-and-capture popup |
| `background.js` | context-menu capture and badge feedback |
| `icons/` | toolbar icons (from `resources/make_icon.py`) |

## Testing against a scratch TaskNest

TaskNest can run with an isolated data folder: `TaskNest.exe --test-profile <empty absolute folder>`. It then uses its own database, integration endpoint and host manifest, and its browser-capture switch registers only under `HKCU\Software\TaskNestTestProfiles\...`. Browsers do not read that location, so this does not install the extension. See "Scratch test profile" in INTEGRATION.md for the table, safety rules and clean-up. Never use a real data folder.
