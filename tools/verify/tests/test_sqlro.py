import hashlib
import sqlite3

import pytest

import sqlro
from core import Status


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@pytest.fixture
def db(tmp_path):
    path = tmp_path / "t.db"
    conn = sqlite3.connect(path)
    conn.executescript("CREATE TABLE tasks(id INTEGER PRIMARY KEY); CREATE TABLE app_metadata(k TEXT);"
                       "CREATE VIEW v AS SELECT id FROM tasks; PRAGMA user_version = 4;")
    conn.commit()
    conn.close()
    return path


def test_inspection_reads_version_and_tables(db):
    res = sqlro.inspect(db)
    assert res.status is Status.PASS
    assert res.user_version == 4
    assert res.tables == ("app_metadata", "tasks")  # views excluded, sorted
    assert res.path == db.resolve()


def test_file_hash_unchanged_and_no_sidecars_created(db):
    before = sha(db)
    names = sorted(p.name for p in db.parent.iterdir())
    for _ in range(3):
        assert sqlro.inspect(db).status is Status.PASS
    assert sha(db) == before
    assert sorted(p.name for p in db.parent.iterdir()) == names


def test_mtime_unchanged(db):
    before = db.stat().st_mtime_ns
    sqlro.inspect(db)
    assert db.stat().st_mtime_ns == before


def test_writes_fail_in_the_read_only_mode(db):
    before = sha(db)
    conn = sqlite3.connect(sqlro._readonly_uri(db.resolve()), uri=True)
    conn.execute("PRAGMA query_only = ON")
    with pytest.raises(sqlite3.OperationalError):
        conn.execute("INSERT INTO tasks(id) VALUES (1)")
    with pytest.raises(sqlite3.OperationalError):
        conn.execute("CREATE TABLE x(a)")
    with pytest.raises(sqlite3.OperationalError):
        conn.execute("PRAGMA user_version = 9")
    conn.close()
    assert sha(db) == before
    assert sqlro.inspect(db).user_version == 4


def test_uri_is_immutable_read_only(db):
    uri = sqlro._readonly_uri(db.resolve())
    assert uri.endswith("?mode=ro&immutable=1") and uri.startswith("file:")


def test_only_fixed_statements_run(db):
    conn = sqlite3.connect(sqlro._readonly_uri(db.resolve()), uri=True)
    for sql in ("DELETE FROM tasks", "PRAGMA user_version = 1", "SELECT * FROM tasks",
                "PRAGMA user_version; DROP TABLE tasks", "pragma user_version"):
        with pytest.raises(ValueError):
            sqlro._run(conn, sql)
    conn.close()


@pytest.mark.parametrize("suffix", ["-wal", "-shm", "-journal"])
def test_sidecar_causes_skip_and_is_untouched(db, suffix):
    side = db.with_name(db.name + suffix)
    side.write_bytes(b"pending")
    before = (sha(db), side.read_bytes())
    res = sqlro.inspect(db)
    assert res.status is Status.SKIP
    assert suffix in res.detail
    assert res.user_version is None and res.tables == ()
    assert (sha(db), side.read_bytes()) == before


def test_real_wal_database_is_skipped(tmp_path):
    path = tmp_path / "w.db"
    conn = sqlite3.connect(path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("CREATE TABLE t(a)")
    conn.commit()
    try:
        assert (tmp_path / "w.db-wal").exists()
        assert sqlro.inspect(path).status is Status.SKIP
    finally:
        conn.close()


@pytest.mark.parametrize("bad", ["", "   ", "a\0b"])
def test_invalid_path_text_fails(bad):
    res = sqlro.inspect(bad)
    assert res.status is Status.FAIL and "invalid" in res.detail


def test_missing_file_fails(tmp_path):
    res = sqlro.inspect(tmp_path / "absent.db")
    assert res.status is Status.FAIL and "does not exist" in res.detail
    assert not (tmp_path / "absent.db").exists()  # not created


def test_directory_fails(tmp_path):
    res = sqlro.inspect(tmp_path)
    assert res.status is Status.FAIL and "regular file" in res.detail


def test_non_sqlite_file_fails_without_change(tmp_path):
    path = tmp_path / "junk.db"
    path.write_bytes(b"this is not a database at all, just text")
    before = sha(path)
    res = sqlro.inspect(path)
    assert res.status is Status.FAIL and "not a SQLite database" in res.detail
    assert sha(path) == before


def test_empty_file_fails(tmp_path):
    path = tmp_path / "empty.db"
    path.write_bytes(b"")
    assert sqlro.inspect(path).status is Status.FAIL
    assert path.read_bytes() == b""


def test_truncated_database_fails_cleanly(db, tmp_path):
    cut = tmp_path / "cut.db"
    cut.write_bytes(db.read_bytes()[:20])
    assert sqlro.inspect(cut).status is Status.FAIL


def test_relative_path_is_resolved(db, monkeypatch):
    monkeypatch.chdir(db.parent)
    res = sqlro.inspect("t.db")
    assert res.status is Status.PASS and res.path == db.resolve()
