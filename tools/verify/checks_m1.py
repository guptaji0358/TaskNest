"""Group "m1": local SQLite schema checks for the approved M1 storage groundwork (Step 3.3).

Database facts come only from `sqlro` (immutable, read-only, query_only). Nothing here
migrates, writes, repairs or creates files. A live database is inspected only when it
exists and has no `-wal`/`-shm`/`-journal` sidecar and no `.lock` file: a lock file means a
TaskNest instance may be running (or crashed), and an immutable read cannot promise a
consistent view of a database that may be changing, so the result is SKIP.

A pass means the structure matches the expected shape. It says nothing about whether the
data is correct, whether sync or conflict handling behaves correctly, or about security.
The source checks scan C++ text and are heuristic.
"""
from __future__ import annotations

import re
from pathlib import Path

import sqlro
from core import KIND_DETERMINISTIC, KIND_HEURISTIC, Registry, Result, Status

GROUP = "m1"
EXPECTED_SCHEMA_VERSION = 4
LIVE_DB_PARTS = ("out", "bin", "data", "tasknest.db")
BACKUP_V3_NAME = "tasknest.db.v3.bak"
BACKUP_V3_VERSION = 3
DATABASE_H = ("scripts", "include", "database.h")
DATABASE_CPP = ("scripts", "src", "database.cpp")

# Expected v4 shape, mirrored from scripts/src/database.cpp. Extra columns are allowed
# (migrations are additive); missing ones are not.
EXPECTED_COLUMNS = {
    "app_metadata": ("key", "value"),
    "tasks": ("id", "title", "notes", "list_name", "priority", "due_at", "completed", "reminder_at",
              "reminder_sent", "created_at", "updated_at", "completed_at", "uuid", "server_rev",
              "held", "held_rev", "held_deleted_at", "not_synced"),
    "sync_state": ("key", "value"),
    "sync_base": ("task_uuid", "rev", "snapshot", "epoch", "trusted"),
    "sync_outbox": ("seq", "op_id", "type", "task_uuid", "setting_key", "base_rev", "after_op_id", "state",
                    "changed", "task", "reason", "value", "authored_rev", "collided", "restore_of",
                    "attempts", "queued_at", "sent_payload"),
    "sync_tombstones": ("task_uuid", "local_id", "server_rev", "deleted_at", "reason", "tombstone_rev",
                        "held_rev", "held_deleted_at"),
    "sync_conflicts": ("id", "record_uid", "source", "kind", "task_uuid", "setting_key", "field_group",
                       "mine_value", "theirs_value", "base_value", "server_rev_seen", "other_device",
                       "detected_at", "change_seq", "delete_reason", "deleted_at", "state",
                       "restore_op_id", "resolved_at", "notified"),
}
SYNC_TABLES = tuple(t for t in EXPECTED_COLUMNS if t.startswith("sync_"))
EXPECTED_PRIMARY_KEYS = {"app_metadata": ("key",), "tasks": ("id",), "sync_state": ("key",),
                         "sync_base": ("task_uuid",), "sync_outbox": ("seq",),
                         "sync_tombstones": ("task_uuid",), "sync_conflicts": ("id",)}
EXPECTED_INDEXES = {"idx_tasks_due_at": "tasks", "idx_tasks_reminder_pending": "tasks",
                    "idx_tasks_completed_at": "tasks", "idx_tasks_uuid": "tasks",
                    "idx_sync_outbox_task": "sync_outbox", "idx_sync_conflicts_uid": "sync_conflicts",
                    "idx_sync_conflicts_local": "sync_conflicts"}


def _res(id: str, status: Status, detail: str = "", kind: str = KIND_DETERMINISTIC) -> Result:
    return Result(GROUP, id, status, detail, kind)


# --- inspection with the extra safety rules ---------------------------------------------------

def inspect_live(ctx) -> sqlro.SchemaInspection:
    """Inspect out/bin/data/tasknest.db, or return SKIP when that is not safe or possible."""
    db = ctx.path(*LIVE_DB_PARTS)
    if not db.is_file():
        return sqlro.SchemaInspection(Status.SKIP, "no live database at out/bin/data/tasknest.db")
    lock = db.with_name(db.name + ".lock")
    if lock.exists():
        return sqlro.SchemaInspection(
            Status.SKIP, "tasknest.db.lock exists (a TaskNest instance may be running); not inspected", db)
    return sqlro.inspect_schema(db)


def _not_inspectable(id: str, insp: sqlro.SchemaInspection, kind: str = KIND_DETERMINISTIC):
    return _res(id, insp.status, insp.detail, kind)


# --- evaluators (pure functions of an inspection; also used by the tests) ---------------------

def eval_version(insp, label: str, expected: int, id: str):
    if insp.status is not Status.PASS:
        return _not_inspectable(id, insp)
    if insp.user_version != expected:
        return _res(id, Status.FAIL, f"{label} user_version is {insp.user_version}, expected {expected}")
    return _res(id, Status.PASS, f"{label} user_version is {expected}")


def eval_tables(insp, id: str = "live_tables"):
    if insp.status is not Status.PASS:
        return _not_inspectable(id, insp)
    missing = [t for t in EXPECTED_COLUMNS if t not in insp.tables]
    if missing:
        return _res(id, Status.FAIL, "missing tables: " + ", ".join(missing))
    return _res(id, Status.PASS, f"all {len(EXPECTED_COLUMNS)} expected tables present "
                                 f"({len(SYNC_TABLES)} sync tables)")


def eval_columns(insp, id: str = "live_columns"):
    if insp.status is not Status.PASS:
        return _not_inspectable(id, insp)
    problems = []
    for table, wanted in EXPECTED_COLUMNS.items():
        cols = insp.columns.get(table)
        if cols is None:
            problems.append(f"{table}: table missing")
            continue
        have = {c.name for c in cols}
        absent = [c for c in wanted if c not in have]
        if absent:
            problems.append(f"{table}: missing columns {', '.join(absent)}")
        pk = tuple(c.name for c in sorted(cols, key=lambda c: c.pk) if c.pk)
        if pk != EXPECTED_PRIMARY_KEYS[table]:
            problems.append(f"{table}: primary key is {pk or 'none'}, expected {EXPECTED_PRIMARY_KEYS[table]}")
    if problems:
        return _res(id, Status.FAIL, "; ".join(problems))
    return _res(id, Status.PASS, "expected columns and primary keys present (names only; extra columns allowed)")


def eval_indexes(insp, id: str = "live_indexes"):
    if insp.status is not Status.PASS:
        return _not_inspectable(id, insp)
    have = dict(insp.indexes)
    problems = [f"{name}: " + ("missing" if name not in have else f"on {have[name]}, expected {table}")
                for name, table in EXPECTED_INDEXES.items() if have.get(name) != table]
    if problems:
        return _res(id, Status.FAIL, "; ".join(problems))
    return _res(id, Status.PASS, f"{len(EXPECTED_INDEXES)} expected indexes present")


# --- registered checks ------------------------------------------------------------------------

def check_live_user_version(ctx):
    return eval_version(inspect_live(ctx), "live database", EXPECTED_SCHEMA_VERSION, "live_user_version")


def check_live_tables(ctx):
    return eval_tables(inspect_live(ctx))


def check_live_columns(ctx):
    return eval_columns(inspect_live(ctx))


def check_live_indexes(ctx):
    return eval_indexes(inspect_live(ctx))


def check_live_schema(ctx):
    """Aggregate verdict on the live schema. PASS only when it was inspected and all four
    live checks pass; INCOMPLETE whenever it could not be verified (missing, locked, sidecar)."""
    insp = inspect_live(ctx)
    if insp.status is Status.SKIP:
        return _res("live_schema", Status.INCOMPLETE,
                    "live schema NOT verified: " + insp.detail)
    if insp.status is Status.FAIL:
        return _res("live_schema", Status.FAIL, insp.detail)
    parts = [eval_version(insp, "live database", EXPECTED_SCHEMA_VERSION, "live_user_version"),
             eval_tables(insp), eval_columns(insp), eval_indexes(insp)]
    failed = [p for p in parts if p.status is not Status.PASS]
    if failed:
        return _res("live_schema", Status.FAIL, "; ".join(f"{p.id}: {p.detail}" for p in failed))
    return _res("live_schema", Status.PASS, "live schema verified: version, tables, columns and indexes match")


def check_backup_v3(ctx):
    backup = ctx.path(*LIVE_DB_PARTS[:-1], BACKUP_V3_NAME)
    if not backup.is_file():
        return _res("backup_v3", Status.SKIP, f"{BACKUP_V3_NAME} not present beside the live database")
    insp = sqlro.inspect(backup)
    return eval_version(insp, "pre-migration backup", BACKUP_V3_VERSION, "backup_v3")


def check_source_schema_version(ctx):
    kind = KIND_HEURISTIC
    path = ctx.path(*DATABASE_H)
    if not path.is_file():
        return _res("source_schema_version", Status.FAIL, "scripts/include/database.h not found", kind)
    match = re.search(r"kSchemaVersion\s*=\s*(\d+)", path.read_text(encoding="utf-8"))
    if not match:
        return _res("source_schema_version", Status.FAIL, "kSchemaVersion not found (regex scan)", kind)
    if int(match.group(1)) != EXPECTED_SCHEMA_VERSION:
        return _res("source_schema_version", Status.FAIL,
                    f"kSchemaVersion is {match.group(1)}, expected {EXPECTED_SCHEMA_VERSION}", kind)
    return _res("source_schema_version", Status.PASS,
                f"kSchemaVersion is {EXPECTED_SCHEMA_VERSION} in database.h (regex scan)", kind)


def check_source_ddl(ctx):
    kind = KIND_HEURISTIC
    path = ctx.path(*DATABASE_CPP)
    if not path.is_file():
        return _res("source_ddl", Status.FAIL, "scripts/src/database.cpp not found", kind)
    text = path.read_text(encoding="utf-8")
    missing = [t for t in EXPECTED_COLUMNS if not re.search(rf"CREATE TABLE IF NOT EXISTS {t}\b", text)]
    missing += [i for i in EXPECTED_INDEXES if not re.search(rf"CREATE (?:UNIQUE )?INDEX IF NOT EXISTS {i}\b", text)]
    if missing:
        return _res("source_ddl", Status.FAIL, "not found in database.cpp (regex scan): " + ", ".join(missing), kind)
    return _res("source_ddl", Status.PASS,
                "CREATE statements for every expected table and index appear in database.cpp (regex scan)", kind)


CHECKS = (
    ("backup_v3", check_backup_v3, KIND_DETERMINISTIC),
    ("live_columns", check_live_columns, KIND_DETERMINISTIC),
    ("live_indexes", check_live_indexes, KIND_DETERMINISTIC),
    ("live_schema", check_live_schema, KIND_DETERMINISTIC),
    ("live_tables", check_live_tables, KIND_DETERMINISTIC),
    ("live_user_version", check_live_user_version, KIND_DETERMINISTIC),
    ("source_ddl", check_source_ddl, KIND_HEURISTIC),
    ("source_schema_version", check_source_schema_version, KIND_HEURISTIC),
)


def register(registry: Registry) -> None:
    for id, func, kind in CHECKS:
        registry.register(GROUP, id, func, kind=kind)
