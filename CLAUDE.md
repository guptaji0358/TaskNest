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
├── scripts/
│   ├── src/        # .cpp implementation files with a matching header in include/
│   ├── include/    # .h/.hpp headers matching files in src/
│   └── main/       # main.cpp and other entry points without a matching project header
└── tests/          # Qt Test suites (built by default; run tst_tasknest and tst_ui from the build dir, or ctest)
```

Rules:

- `scripts/src/` — implementation (`.cpp`) files that have a corresponding header in `scripts/include/`. Application logic lives here.
- `scripts/include/` — project headers (`.h`/`.hpp`) corresponding to files in `scripts/src/`. Declarations and interfaces only.
- `scripts/main/` — `main.cpp` and other entry-point files with no corresponding project header; build or launcher scripts where appropriate. Build outputs go here only if the build configuration explicitly directs them there. Do not put ordinary implementation files or their headers here.
- The folder is named `scripts` but is not limited to executable scripts; follow the conventions above.
- Existing top-level folders such as `resources/` (Qt resource file `app.qrc`, Windows resource file `app.rc`) and `Assets/` are intentional project assets; keep them and add to them only where appropriate.
- `.gitkeep` files are placeholders; remove one once its directory contains real files.
- `tests/` holds automated tests. They must use temporary database files, never the user's real database.
- Do not create unnecessary directories or placeholder source files.

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
- Out-of-source builds only (e.g. `build/`); build directories are git-ignored.
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
