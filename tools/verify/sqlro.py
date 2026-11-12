"""Strictly read-only SQLite inspection for the verification toolkit (Step 3.2).

The database is opened with `mode=ro&immutable=1` and `PRAGMA query_only=ON`. Only
`PRAGMA user_version` and one fixed `sqlite_master` table-name query can run. If a
`-wal`, `-shm` or `-journal` file sits beside the database the file may be in use
or hold uncommitted state that an immutable open would ignore, so the result is SKIP
rather than a possibly wrong answer. Nothing here migrates, writes or creates files.
"""
from __future__ import annotations

import sqlite3
from dataclasses import dataclass, field
from pathlib import Path

from core import Status

SQLITE_MAGIC = b"SQLite format 3\x00"
SIDECAR_SUFFIXES = ("-wal", "-shm", "-journal")

# The complete set of statements this module may execute.
_Q_USER_VERSION = "PRAGMA user_version"
_Q_TABLES = "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name"
_ALLOWED_SQL = frozenset({_Q_USER_VERSION, _Q_TABLES})


@dataclass(frozen=True)
class DbInspection:
    status: Status
    detail: str
    path: Path | None = None
    user_version: int | None = None
    tables: tuple[str, ...] = ()


def _fail(detail: str, path: Path | None = None) -> DbInspection:
    return DbInspection(Status.FAIL, detail, path)


def resolve_db_path(path: Path | str) -> Path:
    """Resolve to an existing regular file. Raises ValueError with a safe message otherwise."""
    text = str(path)
    if not text.strip() or "\0" in text:
        raise ValueError("database path is empty or invalid")
    try:
        resolved = Path(text).resolve(strict=True)
    except (OSError, RuntimeError):
        raise ValueError("database file does not exist") from None
    if not resolved.is_file():
        raise ValueError("database path is not a regular file")
    return resolved


def sidecars(db: Path) -> list[Path]:
    return [db.with_name(db.name + suffix) for suffix in SIDECAR_SUFFIXES
            if db.with_name(db.name + suffix).exists()]


def _has_sqlite_header(db: Path) -> bool:
    with open(db, "rb") as handle:
        return handle.read(len(SQLITE_MAGIC)) == SQLITE_MAGIC


def _readonly_uri(db: Path) -> str:
    return db.as_uri() + "?mode=ro&immutable=1"


def _run(conn: sqlite3.Connection, sql: str) -> list[tuple]:
    if sql not in _ALLOWED_SQL:
        raise ValueError("SQL statement is not allowed")
    return conn.execute(sql).fetchall()


def inspect(path: Path | str) -> DbInspection:
    """Read `PRAGMA user_version` and the table names. Never modifies the database."""
    try:
        db = resolve_db_path(path)
    except ValueError as exc:
        return _fail(str(exc))
    present = sidecars(db)
    if present:
        names = ", ".join(p.name[len(db.name):] for p in present)
        return DbInspection(Status.SKIP,
                            f"database has {names} beside it; not inspected to avoid a stale or racing read", db)
    try:
        if not _has_sqlite_header(db):
            return _fail("file is not a SQLite database", db)
    except OSError as exc:
        return _fail(f"cannot read database file: {type(exc).__name__}", db)
    conn = None
    try:
        conn = sqlite3.connect(_readonly_uri(db), uri=True, timeout=1.0)
        conn.execute("PRAGMA query_only = ON")
        version = _run(conn, _Q_USER_VERSION)[0][0]
        tables = tuple(row[0] for row in _run(conn, _Q_TABLES))
    except sqlite3.Error as exc:
        return _fail(f"cannot read database: {type(exc).__name__}", db)
    finally:
        if conn is not None:
            conn.close()
    return DbInspection(Status.PASS, "read-only inspection ok", db, int(version), tables)


# --- Step 3.3: column and index structure (additive; `inspect` above is unchanged) ----------

# Tables whose columns may be read. Fixed literal statements only; no name is ever formatted in.
SCHEMA_TABLES = ("app_metadata", "sync_base", "sync_conflicts", "sync_outbox",
                 "sync_state", "sync_tombstones", "tasks")
_TABLE_INFO_SQL = {name: f'PRAGMA table_info("{name}")' for name in SCHEMA_TABLES}
_Q_INDEXES = "SELECT name, tbl_name FROM sqlite_master WHERE type = 'index' ORDER BY name"
_ALLOWED_SQL = _ALLOWED_SQL | frozenset(_TABLE_INFO_SQL.values()) | {_Q_INDEXES}


@dataclass(frozen=True)
class Column:
    name: str
    type: str
    notnull: bool
    pk: int


@dataclass(frozen=True)
class SchemaInspection:
    status: Status
    detail: str
    path: Path | None = None
    user_version: int | None = None
    tables: tuple[str, ...] = ()
    columns: dict = field(default_factory=dict)   # table -> tuple[Column, ...] (only tables present)
    indexes: tuple[tuple[str, str], ...] = ()      # (index name, table name)


def inspect_schema(path: Path | str) -> SchemaInspection:
    """`inspect` plus the columns of the known TaskNest tables and all index names. Read-only.

    The first call applies every guard (sidecars, header, immutable read-only open); the
    structure is read only if that call returned PASS.
    """
    base = inspect(path)
    if base.status is not Status.PASS:
        return SchemaInspection(base.status, base.detail, base.path)
    conn = None
    try:
        conn = sqlite3.connect(_readonly_uri(base.path), uri=True, timeout=1.0)
        conn.execute("PRAGMA query_only = ON")
        columns = {}
        for name in SCHEMA_TABLES:
            if name in base.tables:
                rows = _run(conn, _TABLE_INFO_SQL[name])
                columns[name] = tuple(Column(r[1], str(r[2]), bool(r[3]), int(r[5])) for r in rows)
        indexes = tuple((r[0], r[1]) for r in _run(conn, _Q_INDEXES))
    except sqlite3.Error as exc:
        return SchemaInspection(Status.FAIL, f"cannot read database: {type(exc).__name__}", base.path)
    finally:
        if conn is not None:
            conn.close()
    return SchemaInspection(Status.PASS, "read-only inspection ok", base.path, base.user_version,
                            base.tables, columns, indexes)
