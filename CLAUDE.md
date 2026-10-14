# CLAUDE.md — TaskNest

Guidance for Claude (and any contributor) working in this repository. Read this file and inspect the existing code and documentation before making changes.

## Purpose

TaskNest is an offline-first todo application for Windows. Everything is stored locally; there is no account, no cloud backup, and no forced updates.

## Technology Requirements

| Area        | Requirement                                                        |
|-------------|--------------------------------------------------------------------|
| Language    | C++17 or newer                                                     |
| UI          | Qt Quick / QML                                                     |
| Database    | SQLite via Qt SQL (`QSqlDatabase`, `QSqlQuery`, driver `QSQLITE`)  |
| Build       | CMake, paths relative to the repository root                       |
| Platform    | Windows (desktop)                                                  |

## Product Principles

- **Offline-first**: the app must be fully functional with no network connection. Data lives in a local SQLite file.
- **No mandatory account**: no sign-in, registration, or user identity is required.
- **No cloud backup**: do not add cloud sync or remote storage unless explicitly requested.
- **No forced updates**: never auto-update or block usage on an update check.
- **Reminders without unnecessary polling**: schedule reminders against the next due time (e.g. a single-shot timer re-armed on change) rather than periodic polling loops. Do not keep the CPU awake needlessly.
- **Windows startup is optional and disabled by default**: only register for startup when the user explicitly enables it, and remove the registration when they disable it.
- **Never overwrite or delete existing user data without explicit authorization.** Schema migrations must be non-destructive and preserve existing rows; destructive operations require explicit user confirmation.

## Architecture

Keep the implementation modular, with a clear separation of concerns:

- **Data layer** — SQLite access via Qt SQL, schema creation and migrations, repositories. No UI code.
- **Domain/logic layer** — task models, business rules, reminder scheduling. No direct SQL in UI code.
- **UI layer** — QML views and components; C++ types exposed to QML through models and `QObject` controllers.
- **Entry point** — `main.cpp` wires the application together and does as little else as possible.

QML talks to C++ only through clearly defined exposed types (models, controllers); business logic does not live in QML.

## Directory Structure

The repository root is the project root.

```
TaskNest/
├── .git/
├── .gitignore
├── CLAUDE.md
├── CMakeLists.txt
├── scripts/
│   ├── src/        # .cpp implementation files with a matching header in include/
│   ├── include/    # .h/.hpp headers matching files in src/
│   └── main/       # main.cpp, Main.qml, MainView.qml, TaskEditor.qml (entry point and UI)
├── tests/          # Qt Test suites (tst_tasknest, tst_ui)
└── out/            # generated output; git-ignored. out/build is disposable; out/bin/data is NOT
    ├── bin/        # TaskNest.exe (the only application executable) and its data/ folder
    └── build/      # CMake/Ninja build tree, test executables, generated sources
```

Rules:

- `scripts/src/` — implementation (`.cpp`) files that have a corresponding header in `scripts/include/`. Application logic lives here.
- `scripts/include/` — project headers (`.h`/`.hpp`) corresponding to files in `scripts/src/`. Declarations and interfaces only.
- `scripts/main/` — `main.cpp`, the QML files and other entry-point files with no corresponding project header. Do not put ordinary implementation files or their headers here, and never put generated files here.
- `out/` — every generated file (build tree, objects, libraries, executables, test binaries) plus the portable `data/` database folder. Nothing generated may be written to `scripts/`, `tests/` or the repository root.
- **`out/bin/data/` holds the live task database and is not disposable.** Delete or clean only `out/build` (or run `cmake --build out/build --target clean`, which leaves `data/` alone). Deleting `out/` or `out/bin/data/` deletes the user's tasks.
- The folder is named `scripts` but is not limited to executable scripts; follow the conventions above.
- Existing top-level folders such as `resources/` (Qt resource file `app.qrc`, Windows resource file `app.rc`) and `Assets/` are intentional project assets; keep them and add to them only where appropriate.
- `.gitkeep` files are placeholders; remove one once its directory contains real files.
- `tests/` holds automated tests. They must use temporary database files, never the user's real database.
- Do not create unnecessary directories or placeholder source files.

## Task List Behavior

- Default order (`Task::workflowLess`, mirrored by `TaskRepository::SortOrder::Workflow`): open tasks first; then earliest due date (undated last); then higher priority; then creation order. Changing the SQL or the comparator requires changing both.
- The model updates incrementally: completing or re-timing a task *moves* its row; it never rebuilds the list.
- Title search is a literal, Unicode case-insensitive substring match done in C++ (SQLite `LIKE` only folds ASCII). It composes with the view and the completion filter and never changes stored data.
- Keyboard: Ctrl+N, Ctrl+F, Ctrl+1..5 (views), F1 (reference), Down/Up (search/add into the list, between rows), Space, Enter, Delete on a row, Ctrl+Enter to save from notes. Avoid shortcuts that collide with text editing.
- Destructive or repeatable actions (delete confirm, editor save) ignore a second activation while their dialog is closing.

## Quick Add, Undo Delete and Single Instance

- **Quick Add:** the add row is still "type a title, press Enter". The chevron beside Add task (or Ctrl+Shift+N) opens a details panel: priority, list, due date/time, reminder date/time and a one-line note. Details go through the same `TaskListModel::addTask` -> repository -> scheduler path, so reminders are scheduled by the add itself. Invalid dates are refused with the form left intact; a repeat click/Enter right after a successful add is ignored. There is deliberately no free-text syntax (`tomorrow !high`).
- **Undo delete:** after a delete a notice offers Undo for 8 seconds (Ctrl+Z works while it shows and you are not typing). The model keeps **one** in-memory snapshot (the latest delete). Restoring re-inserts the same row with its **original id**, created/updated times and reminder state (`TaskRepository::restoreTask`; AUTOINCREMENT never reuses ids, and an occupied id is refused, not overwritten). A failed undo keeps the snapshot for a retry. The snapshot is discarded when the notice expires or is replaced, and is lost if the app exits first; nothing is written to disk for undo.
- **Single instance:** `SingleInstanceGuard` holds a `QLockFile` named `<database>.lock` beside the database (so separate portable copies do not block each other). A second launch brings the running window forward (Win32) and exits with code 0 without showing a UI. `QLockFile` removes a lock whose owner process no longer exists, so a crash or kill never blocks the next start; a live owner's lock is never treated as stale. If the lock file cannot be created at all (permissions), TaskNest still starts and the database layer reports the real problem. Lock files are git-ignored (`*.lock`).

## Completed-Task Cleanup (permanent deletion, user-configured)

Completed tasks are kept forever unless the user explicitly chooses otherwise.

- **Policy** (`CleanupPolicy`, `scripts/include/cleanuppolicy.h`), saved in `app_metadata` under `completed_cleanup_policy` as `never`, `immediately` or `days:<N>` (N = 1..3650; the UI offers 1, 7, 14 and 30, and shows a saved value outside that list). A missing, unreadable or invalid value means **Never automatically delete**; an invalid stored value is left as it is, not rewritten. The policy is read once at startup (`TaskListModel::loadCleanupPolicy`) and only changes when the user presses Save in **Settings** (sidebar, above the footer).
- **Default is Never.** Upgrading to schema v3 deletes nothing. Selecting a policy that would delete tasks which are *already* completed first shows how many (`previewCleanup`) and needs a second, explicit confirmation ("Save and delete N"); Cancel saves nothing.
- **Immediately:** completing a task deletes it instead (`TaskListModel::completeAndRemove`, one `DELETE`). The row never shows as completed; a database failure leaves the task active and reports the error. Not undoable.
- **After N days:** a completed task is due once `completed_at <= now - N x 24h` (UTC, fixed-width text, compared as text). Tasks with an unknown `completed_at` are never due. Cleanup runs at startup, when a task is completed, when the window is re-activated, and when the policy is saved - never on a timer.
- **Clear completed:** a manual action in Settings (always available, whatever the policy) behind a confirmation dialog; deletes every completed task, never an active one.
- **Permanence:** policy and manual deletions never create an undo snapshot (`m_undo` is only set by `removeTask`). Rows are removed by id from the model; `updateTask`/`setCompleted` on a removed id fail with "not found" and never re-insert. Each deletion emits `tasksMutated`, so `ReminderScheduler::reschedule` drops the timer and any announced reminder (`reminderCancelled`).
- **Database:** `TaskRepository::purgeCompleted` finds and deletes in one transaction with bound parameters and rolls back on any error or row-count mismatch; nothing is vacuumed (SQLite reuses freed pages; there is no automatic `VACUUM`). `tasks.completed_at` (schema v3, indexed for completed rows) is set when a task is completed, kept when it is completed again, cleared when reopened, and preserved by undo-restore. Migration backfilled it from `updated_at` for tasks that were already completed - never earlier than the real completion, so retention can only act later than it should, not sooner.
- Failures are reported through `errorOccurred` and the dialogs; nothing is announced as removed unless the transaction committed.

## Database Location and Safety

- File name: `tasknest.db`. Chosen by `Database::defaultDatabasePath()` in this order:
  1. an existing `<exe folder>/data/tasknest.db` (portable mode);
  2. an existing `%APPDATA%\TaskNest\tasknest.db`;
  3. on first run only: `<exe folder>/data/` if writable, otherwise `%APPDATA%\TaskNest\`.
- An existing database is always reused. If it exists but its folder is not writable, startup fails with an explicit message; TaskNest never quietly creates a second, empty database elsewhere.
- The active path is shown in the sidebar footer (tooltip: full path).
- Upgrading an existing database first copies it to `tasknest.db.v<N>.bak` (once per source version, byte-identical). If the copy cannot be made, the upgrade is not attempted and the database is left untouched.
- Do not keep the live database in a folder that a cloud-sync client (OneDrive, Dropbox) is syncing: SQLite files and sync conflicts do not mix. The development checkout lives under OneDrive, so keep copies of `out/bin/data/` if the data matters.
- Only one TaskNest instance can use a database at a time (see Single Instance above).
- Tests must use temporary databases; never point a test at `out/bin/data/`.

## UI Design System

- Every color, size, radius, spacing step and duration comes from `scripts/main/Theme.qml` (a QML singleton). Do not hard-code them in other QML files.
- Visual direction: graphite surfaces with a cool undertone, one electric-lime accent (actions and the active state only), violet for reminders, amber/red only for time pressure and errors. Urgency is always conveyed by an icon and words as well as color.
- Reusable components are prefixed `TN` (`TNButton`, `TNTextField`, `TNComboBox`, `TNDialog`, ...) plus `Icon` (vector line icons from `Theme.icons`), `TaskRow`, `NavItem`, `TaskCheck`, `ReminderBanner`, `EmptyState`, `CleanupSettings`. Build screens from these rather than styling stock controls inline.
- `scripts/main/qmldir` lists every component so the UI tests can load the QML straight from the source folder; add new components to it **and** to `QML_FILES` in `CMakeLists.txt`.
- Do not name a property `onX` where `X` is another property of the same item: QML reads it as that property's change handler (e.g. `accentText`, not `onAccent`).
- Animations are short (`Theme.durationFast/Base`) and are turned off when Windows animation effects are disabled.
- Keep `objectName`s that the UI tests use (see `tests/qml/`).

## Coding Standards

- C++17 or newer; prefer standard library facilities and Qt idioms (parent-child ownership, signals/slots).
- One class per header/source pair, with matching file names (`TaskRepository.h` ↔ `TaskRepository.cpp`).
- Use include guards or `#pragma once` consistently across headers.
- Use RAII and smart pointers where Qt parent ownership does not apply; avoid raw `new`/`delete` outside Qt ownership.
- Use parameterized SQL (`QSqlQuery::prepare` + `bindValue`); never build SQL by string concatenation with user input.
- Run database access and any long-running work so that the UI thread is not blocked.
- Handle and surface errors (database open failures, query failures) instead of ignoring them.
- Keep QML declarative and free of business logic; keep components small and reusable.
- Match the naming and style of surrounding code; keep comments purposeful rather than redundant.

## Build Expectations

- Build system: CMake, with all paths relative to the repository root (use `CMAKE_SOURCE_DIR` / `CMAKE_CURRENT_SOURCE_DIR`).
- Out-of-source builds only, in `out/build/`; `out/` is git-ignored.
- One configuration, one executable: the app is always `out/bin/TaskNest.exe`. The default build type is Release; configuring the same build directory with `-DCMAKE_BUILD_TYPE=Debug` rebuilds that same file as a Debug build. Do not create per-configuration output folders or duplicate executables.
- Qt and MinGW runtime DLLs are not copied next to the executable; they must be on `PATH` (see below). No installer or deployment package exists yet.
- Build commands, run from the repository root (PATH must contain `D:\Apps\Qt6\Tools\mingw1310_64\bin`, `D:\Apps\Qt6\Tools\Ninja`, `D:\Apps\Qt6\Tools\CMake_64\bin`, and `D:\Apps\Qt6\6.11.1\mingw_64\bin` to run it):

  ```
  cmake -S . -B out/build -G Ninja -DCMAKE_PREFIX_PATH=D:/Apps/Qt6/6.11.1/mingw_64 -DCMAKE_CXX_COMPILER=D:/Apps/Qt6/Tools/mingw1310_64/bin/g++.exe
  cmake --build out/build
  ctest --test-dir out/build --output-on-failure
  ```
- Link only required Qt modules (Core, Gui, Qml, Quick, Sql).
- Do not commit executables, object files, or generated files.
- Do not add third-party dependencies or download libraries without explicit approval.

## Development Rules

1. Inspect existing code and documentation before changing anything; do not assume.
2. Follow the directory structure above and keep modules focused and decoupled.
3. Do not move, rename, or delete existing project files without inspecting their purpose and reporting the plan first.
4. Never overwrite or delete existing user data or local database files without explicit authorization.
5. Do not commit, push, or create releases unless explicitly requested.
6. Keep changes minimal and scoped to the task; do not implement features beyond what was asked.
7. Local database files (`*.db`, `*.sqlite*`) and build artifacts must never be committed.
