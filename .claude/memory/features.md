# Features and behavior

Summary only. The non-obvious rules are authoritative in `.claude/CLAUDE.md` ("Behavior Reference")
and `INTEGRATION.md`. Source file names are verified from `scripts/`.

## Task list and editing

- Default order (`Task::workflowLess`, mirrored by `TaskRepository::SortOrder::Workflow`; change both together).
- Quick Add (`QuickAddPanel.qml`) and the details editor (`TaskEditor.qml`) both go through `TaskListModel::addTask`.
- Title search is literal, Unicode case-insensitive, done in C++ (`tasklistmodel`), never changes data.
- Date/time parsing lives in `scripts/main/DateTimeInput.js`; tested by `tests/qml/tst_datetime.qml`.

## Undo and deletion

- One in-memory snapshot of the latest delete (`TaskRepository::restoreTask`), 8 s notice. Nothing is persisted by undo.
- Completed-task cleanup (`cleanuppolicy.cpp`, `CleanupSettings.qml`) is permanent, default **Never**, and
  never creates an undo snapshot. Runs at startup, on completion, on window activation, and on policy save.
- `completed_at` (schema v3) is set on completion, cleared on reopen.

## Reminders, background and startup

- `ReminderScheduler`: single-shot timer re-armed to the next due time; no polling.
- `Main.qml` + `TrayIcon.qml`: "Keep running in the background" (off by default) hides to tray; reminders use tray messages.
- "Start with Windows" writes `HKCU\...\Run` value `TaskNest` (`shortcutmanager.cpp`); off by default.

## Floating widget

- `FloatingWidget.qml` + `WidgetController` (`widgetcontroller.h/.cpp`): frameless `Qt.Tool`, not in taskbar.
- Preferences in one JSON value `widget_settings` in `app_metadata`, writes debounced 0.4 s.

## Integration (off by default)

- `IntegrationServer` (`integrationserver.cpp`): named-pipe API; no pipe/token/discovery file while disabled.
- `DeepLink::parse` (`deeplink.cpp`) is the only place `tasknest://` is interpreted (`task/<id>`, `view/<name>`, `quickadd`).
- `GlobalHotkey` opens only `QuickCaptureWindow.qml`. `SelectionCapture` reads selections only on explicit request.
- `TaskNestHost.exe` (Native Messaging host) answers `status` and `capture` only; details in `INTEGRATION.md`.

## Scratch test profile

- `--test-profile <root>` (`testprofile.h/.cpp`) redirects DB, lock, discovery and host registration.
  Never point it at real data; cleanup rules are in `.claude/CLAUDE.md`.
