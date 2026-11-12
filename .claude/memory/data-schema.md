# Local database schema

Source of truth: `scripts/src/database.cpp` (schema functions, `kSchemaVersionKey`, `readUserVersion`,
`writeSchemaVersion`). Location rules and backup rules are in `.claude/CLAUDE.md` ("Database Location and Safety").

## File and version tracking

- File: `tasknest.db` (SQLite via `QSQLITE`). Connection owned by `Database` (`scripts/include/database.h`).
- Version recorded twice: `PRAGMA user_version` and `app_metadata.schema_version`. Both are checked at startup.
- Live database `out/bin/data/tasknest.db` is at `user_version` 4 (from `CLAUDE.md`). Pre-migration backup
  `tasknest.db.v3.bak` must be kept.
- Migrations are additive and non-destructive. An upgrade first writes a byte-identical `tasknest.db.v<N>.bak`;
  if that copy fails, the upgrade is not attempted.

## Tables

| Table | Introduced (per `database.cpp` comments and statements) | Purpose |
|---|---|---|
| `app_metadata` | early (created before the tasks table) | key/value settings: `schema_version`, `completed_cleanup_policy`, `widget_settings`, sync state keys. |
| `tasks` | v2 | Task rows. `id` autoincrement integer (device-local). `title` non-empty, `priority` 0–2, `completed` 0/1, `reminder_sent` 0/1. Timestamps are ISO 8601 UTC text. Indexes: `idx_tasks_due_at`, `idx_tasks_reminder_pending`. |
| `tasks.completed_at` | v3 (`ALTER TABLE`) | Set on completion, cleared on reopen, preserved by undo. Index `idx_tasks_completed_at`. |
| `tasks` sync columns | v4 (`ALTER TABLE`) | `uuid`, `server_rev`, `held`, `held_rev`, `held_deleted_at`, `not_synced`. Null `server_rev` = never synced. |
| `sync_state` | v4 | key/value sync state (includes link-generation counter, per `CLOUD.md`). |
| `sync_base` | v4 | Last server copy per task (`task_uuid`, `rev`, full `snapshot`, `epoch`, `trusted`). |
| `sync_outbox` | v4 | Waiting operations: `op_id` unique, `type` in create/update/delete/restore/set_setting, `state` pending/in_flight, `attempts`, `queued_at`, `sent_payload`. Index `idx_sync_outbox_task`. |
| `sync_tombstones` | v4 | Local deletion records keyed by `task_uuid` (no task content). |
| `sync_conflicts` | v4 | Local conflict records (`source` local/server, `kind`, field group, mine/theirs/base values). |

## Rules to keep

- Parameterized SQL only (`prepare` + `bindValue`). PRAGMA `user_version` is the only place a value is
  interpolated, and it is an int constant.
- Migrations must not drop or rewrite user data. Destructive changes need explicit owner approval.
- Sync tables are populated only once an account is linked (`SYNC.md` 11.1); before that TaskNest adds nothing.
- Tests use temporary databases, never the live file.

## Verify before relying on

- The exact step list per version lives in `database.cpp`; this file summarizes it. If they differ, the code wins.
