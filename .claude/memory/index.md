# TaskNest project memory — index

Navigation aid for future sessions. The source code, `CLAUDE.md` and the files
it links (`CLOUD.md`, `INTEGRATION.md`, `api/SYNC.md`, `api/openapi.yaml`) are
authoritative. If a memory file disagrees with the code, trust the code and fix the memory.

Paths below are repo-relative. Read this index first, then only the files relevant to the task.

## Files

| File | Read it when |
|---|---|
| [architecture.md](architecture.md) | You need the layer split, the C++/QML file map or the data flow. |
| [build-and-test.md](build-and-test.md) | You build, run tests, add a source file or touch CMake/QML registration. |
| [features.md](features.md) | You change behavior: task list, reminders, cleanup, undo, widget, tray, deep links, capture. |
| [cloud-and-server.md](cloud-and-server.md) | You touch sync, the `server/` backend, `api/`, or anything about accounts/milestones. |
| [data-schema.md](data-schema.md) | You change the database, migrations, backups or any table. |
| [integration-protocol.md](integration-protocol.md) | You touch the named-pipe API, deep links, the browser extension/native host, or hotkeys. |
| [sync-protocol.md](sync-protocol.md) | You work on sync rules, conflicts, tombstones or the `api/` contract. |
| [conventions-and-decisions.md](conventions-and-decisions.md) | You write code or QML and need the style rules, or why a decision was made. |

## Common task routes

- **New C++ class:** [architecture.md](architecture.md) (layer rules) → [build-and-test.md](build-and-test.md) (register in `CMakeLists.txt`, header/source pair).
- **New or changed QML component:** [architecture.md](architecture.md) (UI rules) → `scripts/main/qmldir` and `QML_FILES` in `CMakeLists.txt`.
- **Database or migration:** [architecture.md](architecture.md) (data layer) → [features.md](features.md) (cleanup/`completed_at`) → `CLAUDE.md` "Database Location and Safety".
- **Reminder or scheduler:** [features.md](features.md) → `scripts/src/reminderscheduler.cpp`.
- **Local API, deep links, browser capture:** [features.md](features.md) → `INTEGRATION.md`.
- **Cloud/sync or server work:** [cloud-and-server.md](cloud-and-server.md) first; never start a milestone without owner approval.
- **Tests:** [build-and-test.md](build-and-test.md).

## Related memories and rules

- Project rules and invariants: `.claude/CLAUDE.md` (authoritative; the memory files summarize, not replace it).
- Cloud design record: `CLOUD.md`. Sync protocol: `api/SYNC.md`.
