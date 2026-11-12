# CLAUDE.md — TaskNest

Guidance for Claude and contributors. Inspect existing code and docs before changing anything. Detail lives in the code and in `INTEGRATION.md` (local API, deep links, browser capture), `CLOUD.md` (cloud decisions/roadmap), `api/SYNC.md` and `api/openapi.yaml` (sync protocol); keep those in step with the code.

## Purpose

Offline-first todo app for Windows. Everything is stored locally; by default no account, no cloud, no forced updates. An **optional, off-by-default cloud sync ("Add Cloud")** is planned (see below); only its local storage groundwork (milestone M1) exists — there is no network sync yet.

## Technology

| Area | Requirement |
|---|---|
| Language | C++17 or newer |
| UI | Qt Quick / QML |
| Database | SQLite via Qt SQL (`QSqlDatabase`, `QSqlQuery`, `QSQLITE`) |
| Build | CMake, paths relative to repo root |
| Platform | Windows desktop (Android planned, cloud milestone M9) |
| Qt | 6.12.0 is the verified environment (the only Qt installed here; build and tests run against it); minimum 6.8 (unverified). Do not change without approval. |
| Cloud (planned) | Python/FastAPI, PostgreSQL, SQLAlchemy 2.x, Alembic, OpenAPI REST+JSON, Docker |

## Product Principles

- **Offline-first**; data in a local SQLite file. **No mandatory account.** **No cloud by default** — only the approved opt-in "Add Cloud"; no other remote storage, telemetry or sync. **No forced updates.**
- **Reminders without polling**: single-shot timer re-armed against the next due time.
- **Windows startup is optional, off by default**; register only on explicit opt-in, remove on opt-out.
- **Never overwrite or delete user data without explicit authorization.** Migrations are non-destructive; destructive operations need user confirmation.

## Architecture

- **Data layer**: SQLite access, schema/migrations, repositories. No UI code.
- **Domain layer**: task models, business rules, reminder scheduling. No SQL in UI code.
- **UI layer**: QML views/components; C++ exposed to QML only via models and `QObject` controllers. No business logic in QML.
- **Entry point**: `main.cpp` only wires things together.

## Directory Structure

```
TaskNest/
├── CLAUDE.md, CLOUD.md, INTEGRATION.md, CMakeLists.txt
├── api/          # cloud contract only: openapi.yaml, SYNC.md, conformance/ (no code; LF, UTF-8, no BOM)
├── server/       # planned FastAPI backend (milestone M2); venv/caches/test output go under out/server/
├── browser-extension/   # MV3 extension
├── resources/    # app.qrc, app.rc, app.ico, app.manifest, runtime_manifest.cmake, make_icon.py
├── scripts/
│   ├── src/      # .cpp with a matching header in include/
│   ├── include/  # .h/.hpp matching src/
│   └── main/     # main.cpp, QML files, qmldir, DateTimeInput.js (no matching project header)
├── tests/        # Qt Test suites (tst_tasknest, tst_sync, tst_ui, qml/)
└── out/          # generated, git-ignored
    ├── bin/      # TaskNest.exe, TaskNestHost.exe, qt.conf, data/ (LIVE), runtime/ (generated)
    └── build/    # CMake/Ninja tree (disposable)
```

- Nothing generated may be written outside `out/`. Never put generated files in `scripts/`, `tests/` or the root.
- **`out/bin/data/` holds the live database and is NOT disposable.** Clean only `out/build` (`cmake --build out/build --target clean` leaves `data/` alone). Deleting `out/` or `out/bin/data/` deletes the user's tasks.
- `.gitkeep` files are placeholders; remove once the directory has real files. Do not create unnecessary directories or placeholder sources.
- Tests use temporary databases, never `out/bin/data/`.

## Build

- Out-of-source in `out/build/`; default Release; Debug via `-DCMAKE_BUILD_TYPE=Debug` in the same tree. One app executable `out/bin/TaskNest.exe` (+ `TaskNestHost.exe`); no per-config output folders.
- Build deploys the Qt/MinGW runtime into `out/bin/runtime/` (windeployqt) via an embedded private side-by-side manifest; `runtime/` is disposable. `-DTASKNEST_DEPLOY_DLLS=OFF` embeds no manifest (Qt/MinGW must be on `PATH`).
- PATH needs `D:\Apps\Qt6\Tools\mingw1310_64\bin`, `D:\Apps\Qt6\Tools\Ninja`, `D:\Apps\Qt6\Tools\CMake_64\bin`, `D:\Apps\Qt6\6.12.0\mingw_64\bin`. From the repo root:

  ```
  cmake -S . -B out/build -G Ninja -DCMAKE_PREFIX_PATH=D:/Apps/Qt6/6.12.0/mingw_64 -DCMAKE_CXX_COMPILER=D:/Apps/Qt6/Tools/mingw1310_64/bin/g++.exe
  cmake --build out/build
  ctest --test-dir out/build --output-on-failure
  ```
- Link only needed Qt modules (Core, Gui, Qml, Quick, Sql). No new third-party dependencies or downloads without approval.
- The C++ build and CTest MUST NOT require Python, Docker or PostgreSQL. Server tests are separate and off by default.
- Do not install PostgreSQL, Docker, extra Qt versions or kits (incl. Android) without approval.
- Do not commit executables, objects or generated files.

## Coding Standards

- C++17+; standard library and Qt idioms (parent-child ownership, signals/slots); RAII/smart pointers where Qt ownership doesn't apply; no raw `new`/`delete` outside Qt ownership.
- One class per header/source pair with matching names; `#pragma once` consistently.
- Parameterized SQL only (`prepare` + `bindValue`); never concatenate user input.
- Keep long-running work off the UI thread. Surface errors; don't ignore them.
- Match surrounding naming/style; comments purposeful, not redundant. QML declarative and small/reusable.

## UI Design System

- Every color, size, radius, spacing, duration comes from the `Theme.qml` singleton; never hard-code in other QML.
- Direction: graphite surfaces, one electric-lime accent (actions/active state), violet for reminders, amber/red only for time pressure and errors. Urgency is conveyed by icon and words as well as color.
- Reusable components are `TN`-prefixed (`TNButton`, `TNTextField`, `TNDialog`, `TNDateField`, `TNTimeField`, ...) plus `Icon`, `TaskRow`, `NavItem`, `TaskCheck`, `TNPopupSurface` (all popups), `Atmosphere`, etc. Build screens from these.
- New components go in `scripts/main/qmldir` **and** `QML_FILES` in `CMakeLists.txt`.
- Do not name a property `onX` where `X` is another property (read as a change handler); e.g. `accentText`, not `onAccent`.
- Motion only from `Theme` tokens; Windows animation-off or Settings → Reduce motion (`reduceMotion` in `widget_settings`) zeroes all durations. Animations are decoration only; state changes immediately; no looping animations.
- Keep `objectName`s used by UI tests (`tests/qml/`). Every top-level window must accept `onClosing`; a vetoing hidden window keeps the process alive.

## Behavior Reference (code is the source of truth; these are the non-obvious rules)

**Task list**
- Default order (`Task::workflowLess` mirrored by `TaskRepository::SortOrder::Workflow`; change both together): open first; earliest due (undated last); higher priority; creation order.
- Model updates incrementally (rows move, never rebuild).
- Title search: literal, Unicode case-insensitive substring in C++ (SQLite `LIKE` folds only ASCII); composes with view/filter; never changes data.
- Shortcuts: Ctrl+N, Ctrl+F, Ctrl+Shift+W (widget), Ctrl+Shift+N (quick-add details), Ctrl+1..5 (views), F1, Ctrl+Z (undo), Ctrl+Enter (save notes). Avoid text-editing collisions.
- Destructive/repeatable actions ignore a second activation while their dialog is closing.

**Quick Add / Undo / Single instance**
- Quick Add: type a title + Enter; chevron or Ctrl+Shift+N opens details (priority, list, due, reminder, note) through the same `TaskListModel::addTask` path. Invalid dates refuse with form intact. No free-text syntax.
- Undo: one in-memory snapshot of the latest delete, 8 s notice; `TaskRepository::restoreTask` re-inserts with original id/timestamps/reminder state, refuses an occupied id; failed undo keeps the snapshot; nothing persisted.
- `SingleInstanceGuard`: `QLockFile` `<database>.lock` beside the DB; second launch brings the window forward (or forwards a deep link) and exits 0 without UI; stale locks self-clear; if the lock can't be created, still start. `*.lock` is git-ignored.

**Completed-task cleanup (permanent deletion, user-configured)**
- Policy (`CleanupPolicy`) in `app_metadata` `completed_cleanup_policy`: `never` | `immediately` | `days:<N>` (1..3650). Missing/invalid = **Never**; invalid values are not rewritten. Read at startup; changes only when the user presses Save in Settings.
- **Default Never.** A policy that would delete already-completed tasks first shows the count (`previewCleanup`) and needs a second explicit confirmation; Cancel saves nothing.
- Immediately: completing deletes (`completeAndRemove`, one DELETE, not undoable; a DB failure leaves the task active). After N days: due when `completed_at <= now - N×24h` (UTC fixed-width text); unknown `completed_at` is never due. Runs at startup, on completion, on window re-activation, on policy save — never on a timer.
- "Clear completed" in Settings: always available, confirmation dialog, completed tasks only.
- Cleanup deletions never create an undo snapshot; `updateTask`/`setCompleted` on a removed id fail "not found" and never re-insert; each emits `tasksMutated` (scheduler drops timers/announced reminders).
- `purgeCompleted`: find+delete in one transaction, bound params, rollback on error or row-count mismatch; no automatic VACUUM. Report only committed removals. `completed_at` (schema v3) is set on completion, kept on re-complete, cleared on reopen, preserved by undo.

**Date/time input** (`scripts/main/DateTimeInput.js`, tested by `tests/qml/tst_datetime.qml`)
- Results are local-time `Date`s; C++ converts to UTC; stored format unchanged.
- Dates `YYYY-MM-DD` (also `/`, `.`, 1-digit month/day); times 24h `H:mm`, `HH:mm`, `HH:mm:ss`, `HH-mm`, `HH-mm-ss`; years 1000–9999. Valid entry is normalized on blur. Invalid entry is never changed or rolled into another day: keep text, danger outline, show the reason.
- Date-only defaults: due 23:59, reminder 09:00; time field stays blank until set.

**Floating widget** (`FloatingWidget.qml`, `WidgetController`)
- Frameless `Qt.Tool` window, not in taskbar, on top by default, `transientParent: null`. Never registered for startup. Shown at launch only if `showOnLaunch` is on.
- All task changes go through the main `TaskListModel`; its `tasksMutated` refreshes the widget views. No timers.
- Preferences: one JSON value `widget_settings` in `app_metadata`; writes debounced 0.4 s, flushed on shutdown; bad values fall back to defaults, numbers clamped. Off-screen saved position is recovered for display but the raw position is kept until the user moves it.

**Background / tray / startup**
- Closing the main window quits unless Settings → "Keep running in the background" (`runInBackground`, off by default): then it hides to a tray icon (`TrayIcon.qml`); reminders raise a tray message; if no tray, closing quits. `quitOnLastWindowClosed` is false; `Main.qml` decides when the process ends. `prepareForShutdown()` writes preferences first.
- "Start with Windows" (off by default, requires background option): one per-user `HKCU\...\Run` value `TaskNest` = `"<exe>" --background` (`ShortcutManager::setStartWithWindows`), removed when off; refused in a test profile. Start Menu shortcut / taskbar-pin helper also in `ShortcutManager`.

**Integration endpoint, deep links, browser capture** (details: `INTEGRATION.md`)
- `IntegrationServer`: local named-pipe API, **off by default** (`integration_enabled`); while off there is no pipe/token/discovery file. Operations go through `TaskListModel`/`TaskRepository`; UI requests are signals handled in `Main.qml`; never launches TaskNest; no timers beyond a per-connection 5 s cut-off. Tests use `setDiscoveryDirectory`, never `%LOCALAPPDATA%\TaskNest\integration`.
- `DeepLink::parse` is the only place `tasknest://` is interpreted (`task/<id>`, `view/<name>`, `quickadd`); links carry no content/token; refusals never echo the link. Scheme registered under HKCU only when enabled in Settings (`UrlSchemeRegistration`). A second launch forwards via `WM_COPYDATA` and never opens the DB.
- `GlobalHotkey` (`RegisterHotKey`, off by default) opens only `QuickCaptureWindow.qml`. `SelectionCapture` reads the selection via UI Automation on explicit request only (key `global_capture_selection`, off by default); stores nothing until confirmed; no keystroke injection, clipboard access or password fields.
- `TaskNestHost.exe`: Native Messaging host (Qt Core + Network), answers once and exits, never starts TaskNest, never returns the credential; only `status` and `capture`. Extension id fixed by manifest `key` and must equal `NativeHost::kExtensionId` (tested, as is the minimal permission set). Registration only from Settings (HKCU, off by default) via `NativeHostRegistration`; registry *reads* use read-only `RegistryUtil` (`QSettings` creates keys). Host reads stdin unbuffered.
- `capture_page` optional `request_id` is idempotent for 10 min. Once a `capture` is written to the pipe, a missing answer is `outcome_unknown`, never a plain failure; popup keeps one request id per capture and re-sends it on "Check again".

## Database Location and Safety

- File `tasknest.db`, chosen by `Database::defaultDatabasePath()`: (1) existing `<exe folder>/data/tasknest.db` (portable); (2) existing `%APPDATA%\TaskNest\tasknest.db`; (3) first run: `<exe folder>/data/` if writable, else `%APPDATA%\TaskNest\`.
- An existing DB is always reused; if its folder isn't writable, startup fails explicitly — never silently create a second empty DB. Active path shown in the sidebar footer.
- Upgrades first copy to `tasknest.db.v<N>.bak` (once per source version, byte-identical); if the copy fails the upgrade is not attempted.
- Don't keep the live DB in a cloud-synced folder (OneDrive, Dropbox). The dev checkout is under OneDrive, so keep copies of `out/bin/data/`.
- One TaskNest instance per database.

## Optional Cloud Sync (approved architecture; M1 local storage only)

Reasoning/risks: `CLOUD.md`. Protocol: `api/SYNC.md`, `api/openapi.yaml`. If code and these disagree, stop and resolve it. Invariants:

- **Off by default**, opt-in "Add Cloud" in Settings. Off or unlinked → never touches the network for sync. Local settings (incl. local cleanup policy) are never overwritten by cloud state.
- **SQLite stays the source of truth.** Additive, non-destructive migrations (`user_version` 3→4+) with the existing byte-copy backup first; extra byte-identical backup before first account link and before any sync-driven local deletion.
- **API contract is owned by TaskNest** (`api/`); no provider-specific SDK/host/field/DB logic; server URL is user-editable; backend runs on any standard PostgreSQL/Docker host.
- **Identity/ordering:** client-generated UUID per task (local integer `id` stays device-local); server alone orders changes (per-account sequence), client clocks never decide; ops idempotent by `op_id`; writes via a transactional outbox in the same transaction as the local change.
- **Deletion/conflicts:** all deletions sync as tombstones (incl. retention purges); a deliberate user edit resurrects a deleted task ("edit wins", user notified), holding an old copy never does; conflicts preserved and shown (`SYNC.md` §6–7).
- **Cleanup policy:** one account-wide policy when linked (`SYNC.md` §8).
- **Accounts:** login name + Argon2id password + ten single-use recovery codes; no email in v1; losing both = cloud account unrecoverable (local data unaffected); no auto-deletion of inactive accounts.
  - Passwords: NFC-normalized, 15–128 Unicode code points, case/whitespace preserved, no composition rules or periodic change, local common-password blocklist (no external API). Login identifiers: NFKC + casefold.
  - Sessions: refresh tokens slide 90 days, absolute limit 180 days from family creation; rotation with reuse detection. Strict rotation, no grace window: a lost refresh response means re-login. Access tokens stop working the moment their family expires or is revoked (`CLOUD.md` §16.4).
  - Devices: max 25 active; the 26th sign-in gets `409 device_limit_reached`; never auto-revoke. Device list/revoke belong to M2.
- **Secrets:** tokens in the OS secure store (Credential Manager; Android Keystore), never in SQLite, settings, logs or command lines. Task content never logged.
- **Event-driven sync:** debounced push after local change, pull on activation, explicit "Sync now", single-shot backoff timers only while work is pending; no periodic polling; failures never block local use.
- **Tests:** server tests use a local PostgreSQL with a dedicated test DB; Qt tests use temporary SQLite and a fake/local server; sync is refused under `--test-profile` against any real server. Both suites run `api/conformance/` fixtures in place.

Delivery is by milestone M0–M10 (`CLOUD.md` §10); **each starts only after the owner explicitly approves it.** M0 (contract, rules, fixtures, docs) is complete. M1 (local storage groundwork: schema, UUIDs, bases, tombstones, outbox, transactional mutations, conflict table; no network code, `SyncStore`) was explicitly approved by the owner and is in progress — its schema changes are additive. The live database (`out/bin/data/tasknest.db`) is already at schema v4 (`user_version` 4): a migration has occurred, but who or what ran it is unknown (not recorded; Claude did not run it). The pre-migration backup `tasknest.db.v3.bak` (schema v3) remains available beside it; keep it. Sync itself (network, server, UI, account linking) is not implemented. **M2 (server skeleton: accounts, sessions, recovery, devices only) is approved in plan; only its documentation step M2.0 is approved so far.** M2 contains no sync tables or endpoints: `processed_ops`, `conflicts`, migration 0002 and the 90-day retention purge (with its database tests) belong to **M3**. The 90-day policy is documented now: deleted task content can remain in conflict history for up to 90 days. `api/` changes for M2 need separate owner approval of the shown diff. No M2.1+ work, dependency, PostgreSQL or Docker installation without explicit approval (`CLOUD.md` §16). Do not start the next milestone, run a migration, install software, or modify application source beyond the approved milestone's scope.

## Development Rules

1. Inspect existing code and docs first; do not assume.
2. Follow the directory structure; keep modules focused and decoupled.
3. Do not move, rename or delete existing project files without inspecting their purpose and reporting the plan first.
4. Never overwrite or delete user data or local database files without explicit authorization.
5. Do not commit, push or create releases unless explicitly requested.
6. Keep changes minimal and scoped; no features beyond what was asked.
7. Never commit `*.db`, `*.sqlite*` or build artifacts.
8. Cloud work: one approved milestone at a time (see above).

## Scratch Test Profile (`--test-profile <root>`)

Acceptance testing only; selected solely by the command-line option (never env var, setting or browser message). **Never point it at real data** (`out/bin/data`, `%APPDATA%\TaskNest`, `%LOCALAPPDATA%\TaskNest`); those are refused. Invalid option → exit code 2 before touching any database.

- `<root>` rules (`TestProfile`, `scripts/include/testprofile.h`): absolute local drive path, no `.`/`..`, not a drive root or UNC, must not equal/contain/sit inside the exe folder, `<exe>\data`, `%APPDATA%\TaskNest`, `%LOCALAPPDATA%\TaskNest`. TaskNest creates the folder (plus `data\`) and marker `tasknest-test-profile.json`; a non-empty folder without the marker is refused.
- Redirected: DB+settings → `<root>\data\tasknest.db`; lock beside it; integration discovery → `<root>\integration`; host manifest/launcher → `<root>\native-host`; browser-host registry keys → `HKCU\Software\TaskNestTestProfiles\<16 hex of root>\...`. `tasknest://` registration and Start Menu shortcut are refused.
- The host (`TaskNestHost.exe --test-profile "<root>"`) uses only `<root>\integration` and requires the marker; a missing/unmarked profile or unexpected argument answers `configuration`. Tokens are never on a command line.
- Limitation: browsers read only the real `NativeMessagingHosts` keys, so a profile does not make Chrome/Edge discover the host.
- "Is TaskNest running?" (`NativeHost::isInstanceRunning`) reads the served database's `.lock` and requires a live `TaskNest.exe` owner, so a scratch instance is never mistaken for the normal one.
- Clean up: close the scratch instance (turn browser capture off first, or delete `HKCU\Software\TaskNestTestProfiles\<hash>`), then delete `<root>` only.

## Testing and Reporting

- `tests/RunQtTest.cmake` wraps the Qt Test executables (GUI-subsystem, no console): writes `out/build/tests/*.log`, prints failures/skips and `SUMMARY: N passed, N failed, N skipped`, and fails the CTest entry as **INCOMPLETE** when skips exceed the budget (`TASKNEST_SKIP_BUDGET`, `TASKNEST_UI_SKIP_BUDGET`; default 0). A green entry means every test ran. Wrapper covered by `qt_test_wrapper` (`RunQtTestSelfTest.cmake`, fake `tst_fakeqt`).
- UI tests skip only after positively detecting that the window cannot get OS foreground activation. `tst_ui.qml` `cleanup()` destroys the view and waits until it and its dialogs (in the window overlay) are gone — `find()` returns the first match, so a stale dialog would take the next test's input.
- `tst_tasknest` starts disposable processes under scratch profiles (`tst_lockowner`, real `TaskNest.exe --test-profile` with `QT_QPA_PLATFORM=offscreen`); they are killed even on failure and never use the live database.
