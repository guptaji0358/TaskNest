# Architecture

Verified against `CMakeLists.txt`, `scripts/` and `.claude/CLAUDE.md` (Architecture section).

## Layers (rules from `.claude/CLAUDE.md`)

- **Data layer** — SQLite via Qt SQL. `Database` (`scripts/include/database.h`, `scripts/src/database.cpp`)
  opens the connection and owns migrations/backup; `TaskRepository` (`taskrepository.h/.cpp`) is the
  only SQL for tasks; `SyncStore` (`syncstore.h/.cpp`) is M1 sync-groundwork storage with no network code.
- **Domain layer** — `Task` (`scripts/include/task.h`, ordering `workflowLess`), `CleanupPolicy`
  (`cleanuppolicy.h/.cpp`), `ReminderScheduler` (`reminderscheduler.h/.cpp`, single-shot timer).
- **UI layer** — QML in `scripts/main/`. C++ reaches QML only through models and `QObject`
  controllers: `TaskListModel` (`tasklistmodel.h/.cpp`), `WidgetController`, `IntegrationServer`, etc.
  No business logic in QML; no SQL in UI code.
- **Entry point** — `scripts/main/main.cpp` only wires things together (single-instance lock,
  database path, controllers, QML engine). `scripts/main/hostmain.cpp` is the Native Messaging host.

## Build targets (from `CMakeLists.txt`)

| Target | Contents |
|---|---|
| `tasknesthostcore` (static) | Qt Core + Network code for the native host. |
| `tasknestcore` (static) | All `scripts/src/*.cpp`; public include dir `scripts/include`. Links Qt Core/Gui/Qml/Quick/Sql (and `dwmapi` on Windows). |
| `appTaskNest` (executable, `out/bin/TaskNest.exe`) | `main.cpp` + QML module (`qt_add_qml_module`). |
| `TaskNestHost` (executable) | `scripts/main/hostmain.cpp`, links `tasknesthostcore`. |

Tests and helpers are described in [build-and-test.md](build-and-test.md).

## Key data flow

1. A user action in QML calls a `TaskListModel` slot (or a controller slot).
2. `TaskListModel` calls `TaskRepository`, which runs parameterized SQL on the `Database` connection.
3. The model updates its rows incrementally and emits `tasksMutated`; widget views and the reminder scheduler react.
4. Settings persist through `app_metadata` (e.g. `completed_cleanup_policy`, `widget_settings`).

## Verified invariants worth remembering

- `SingleInstanceGuard` (`singleinstance.cpp`) uses a `QLockFile` beside the database; one instance per DB.
- `main.cpp` sets `setQuitOnLastWindowClosed(false)`; `Main.qml` decides when the process ends.
- The live database is `out/bin/data/tasknest.db` and is not disposable (see `.claude/CLAUDE.md`).
- Undo, cleanup and sync deletes follow the rules in `.claude/CLAUDE.md` "Behavior Reference"; do not re-derive them.

## Open questions

- Whether `SyncStore` is wired into `TaskRepository` mutations end-to-end is not verified here; check
  `scripts/src/taskrepository.cpp` and `tests/tst_sync.cpp` before relying on it.
