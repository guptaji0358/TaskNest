import hashlib
import sqlite3

import pytest

import checks_m1 as m1
import core
import repo
import sqlro
import verify
from core import Status


def build_db(path, version=4, drop_table=None, drop_column=None, drop_index=None, wal=False):
    conn = sqlite3.connect(path)
    for table, cols in m1.EXPECTED_COLUMNS.items():
        if table == drop_table:
            continue
        pk = m1.EXPECTED_PRIMARY_KEYS[table]
        defs = [f"{c} INTEGER PRIMARY KEY" if (c in pk and len(pk) == 1 and c in ("id", "seq"))
                else f"{c} TEXT PRIMARY KEY" if c in pk else f"{c} TEXT"
                for c in cols if (table, c) != drop_column]
        conn.execute(f"CREATE TABLE {table} ({', '.join(defs)})")
    for name, table in m1.EXPECTED_INDEXES.items():
        if name == drop_index or table == drop_table:
            continue
        col = "uuid" if table == "tasks" and name == "idx_tasks_uuid" else m1.EXPECTED_COLUMNS[table][0]
        if (table, col) == drop_column:
            continue
        conn.execute(f"CREATE INDEX {name} ON {table} ({col})")
    conn.execute(f"PRAGMA user_version = {version}")
    if wal:
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("INSERT INTO app_metadata (key, value) VALUES ('k', 'v')")
    conn.commit()
    if not wal:
        conn.close()
    return conn if wal else None


@pytest.fixture
def project(fake_repo):
    data = fake_repo / "out" / "bin" / "data"
    data.mkdir(parents=True)
    return fake_repo, data / "tasknest.db"


def ctx_for(root):
    return repo.Ctx(root=root)


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file()}


def run_all(root):
    reg = core.Registry()
    m1.register(reg)
    return {r.id: r for r in core.run_plan(core.build_plan(reg, [], False), ctx_for(root))}


def test_good_live_database_passes_every_live_check(project):
    root, db = project
    build_db(db)
    res = run_all(root)
    for id in ("live_user_version", "live_tables", "live_columns", "live_indexes"):
        assert res[id].status is Status.PASS, res[id]
        assert res[id].kind == "deterministic"


def test_no_live_database_skips(project):
    root, _ = project
    res = run_all(root)
    assert all(res[i].status is Status.SKIP for i in
               ("live_user_version", "live_tables", "live_columns", "live_indexes", "backup_v3"))
    assert "no live database" in res["live_tables"].detail


@pytest.mark.parametrize("version", [3, 5, 0])
def test_wrong_user_version_fails(project, version):
    root, db = project
    build_db(db, version=version)
    r = run_all(root)["live_user_version"]
    assert r.status is Status.FAIL and f"is {version}" in r.detail


def test_missing_sync_table_fails(project):
    root, db = project
    build_db(db, drop_table="sync_outbox")
    res = run_all(root)
    assert res["live_tables"].status is Status.FAIL and "sync_outbox" in res["live_tables"].detail
    assert res["live_columns"].status is Status.FAIL


def test_missing_column_fails_and_extra_column_is_allowed(project):
    root, db = project
    build_db(db, drop_column=("tasks", "not_synced"))
    r = run_all(root)["live_columns"]
    assert r.status is Status.FAIL and "not_synced" in r.detail
    db.unlink()
    build_db(db)
    conn = sqlite3.connect(db)
    conn.execute("ALTER TABLE tasks ADD COLUMN future_col TEXT")
    conn.commit()
    conn.close()
    assert run_all(root)["live_columns"].status is Status.PASS


def test_missing_index_fails(project):
    root, db = project
    build_db(db, drop_index="idx_sync_outbox_task")
    r = run_all(root)["live_indexes"]
    assert r.status is Status.FAIL and "idx_sync_outbox_task" in r.detail


def test_wal_sidecar_skips_and_nothing_is_touched(project):
    root, db = project
    conn = build_db(db, wal=True)  # connection stays open, so -wal/-shm exist
    try:
        assert db.with_name(db.name + "-wal").exists()
        before = snapshot(root)
        res = run_all(root)
        assert res["live_user_version"].status is Status.SKIP
        assert "-wal" in res["live_user_version"].detail
        assert snapshot(root) == before
    finally:
        conn.close()


def test_lock_file_skips_without_reading_the_database(project):
    root, db = project
    build_db(db)
    db.with_name(db.name + ".lock").write_text("pid", encoding="utf-8")
    before = snapshot(root)
    res = run_all(root)
    assert res["live_tables"].status is Status.SKIP and ".lock" in res["live_tables"].detail
    assert snapshot(root) == before


def test_corrupt_live_database_fails(project):
    root, db = project
    db.write_bytes(b"this is not sqlite" * 10)
    assert run_all(root)["live_tables"].status is Status.FAIL


def test_checks_leave_database_byte_identical_and_create_no_files(project):
    root, db = project
    build_db(db)
    build_db(db.with_name("tasknest.db.v3.bak"), version=3)
    before = snapshot(root)
    names = sorted(p.name for p in db.parent.iterdir())
    for _ in range(2):
        res = run_all(root)
        assert res["backup_v3"].status is Status.PASS
    assert snapshot(root) == before
    assert sorted(p.name for p in db.parent.iterdir()) == names


def test_backup_with_wrong_version_fails(project):
    root, db = project
    build_db(db.with_name("tasknest.db.v3.bak"), version=4)
    assert run_all(root)["backup_v3"].status is Status.FAIL


def test_schema_inspection_never_runs_other_sql(project):
    _, db = project
    build_db(db)
    insp = sqlro.inspect_schema(db)
    assert insp.status is Status.PASS and set(insp.columns) == set(m1.EXPECTED_COLUMNS)
    with pytest.raises(ValueError):
        sqlro._run(sqlite3.connect(":memory:"), "DROP TABLE tasks")


def source_tree(root, version="4", tables=True, indexes=True):
    (root / "scripts" / "include").mkdir(parents=True)
    (root / "scripts" / "src").mkdir(parents=True)
    (root / "scripts" / "include" / "database.h").write_text(
        f"static constexpr int kSchemaVersion = {version};\n", encoding="utf-8")
    lines = []
    if tables:
        lines += [f'"CREATE TABLE IF NOT EXISTS {t} ("' for t in m1.EXPECTED_COLUMNS]
    if indexes:
        lines += [f'"CREATE INDEX IF NOT EXISTS {i} ON x"' for i in m1.EXPECTED_INDEXES]
    (root / "scripts" / "src" / "database.cpp").write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_source_checks_pass_and_are_heuristic(fake_repo):
    source_tree(fake_repo)
    res = run_all(fake_repo)
    for id in ("source_schema_version", "source_ddl"):
        assert res[id].status is Status.PASS and res[id].kind == "heuristic"


def test_source_checks_detect_wrong_version_and_missing_ddl(fake_repo):
    source_tree(fake_repo, version="5", tables=False)
    res = run_all(fake_repo)
    assert res["source_schema_version"].status is Status.FAIL
    assert res["source_ddl"].status is Status.FAIL and "sync_base" in res["source_ddl"].detail


def test_source_checks_fail_when_files_missing(fake_repo):
    res = run_all(fake_repo)
    assert res["source_schema_version"].status is Status.FAIL and res["source_ddl"].status is Status.FAIL


def test_kinds_split_between_deterministic_and_heuristic():
    reg = core.Registry()
    m1.register(reg)
    kinds = {s.id: s.kind for s in reg.specs()}
    assert {i for i, k in kinds.items() if k == "heuristic"} == {"source_ddl", "source_schema_version"}
    assert all(i.startswith(("live_", "backup_")) for i, k in kinds.items() if k == "deterministic")


# --- aggregate live-schema verdict -----------------------------------------------------------

def test_aggregate_passes_for_safely_inspectable_database(project):
    root, db = project
    build_db(db)
    r = run_all(root)["live_schema"]
    assert r.status is Status.PASS and "verified" in r.detail and r.kind == "deterministic"


def test_aggregate_is_incomplete_when_database_missing(project):
    root, _ = project
    r = run_all(root)["live_schema"]
    assert r.status is Status.INCOMPLETE and "NOT verified" in r.detail


def test_aggregate_is_incomplete_when_locked_and_individual_checks_stay_skip(project):
    root, db = project
    build_db(db)
    db.with_name(db.name + ".lock").write_text("pid", encoding="utf-8")
    before = snapshot(root)
    res = run_all(root)
    assert res["live_schema"].status is Status.INCOMPLETE and ".lock" in res["live_schema"].detail
    for id in ("live_user_version", "live_tables", "live_columns", "live_indexes"):
        assert res[id].status is Status.SKIP
    assert (db.with_name(db.name + ".lock")).exists()  # the lock is never removed
    assert snapshot(root) == before


def test_aggregate_is_incomplete_with_wal_sidecar(project):
    root, db = project
    conn = build_db(db, wal=True)
    try:
        assert run_all(root)["live_schema"].status is Status.INCOMPLETE
    finally:
        conn.close()


def test_aggregate_fails_on_wrong_structure_or_corruption(project):
    root, db = project
    build_db(db, drop_table="sync_state")
    assert run_all(root)["live_schema"].status is Status.FAIL
    db.write_bytes(b"not sqlite" * 20)
    assert run_all(root)["live_schema"].status is Status.FAIL


def test_backup_v3_is_independent_of_live_state(project):
    root, db = project
    build_db(db.with_name("tasknest.db.v3.bak"), version=3)
    res = run_all(root)  # no live database at all
    assert res["live_schema"].status is Status.INCOMPLETE and res["backup_v3"].status is Status.PASS
    build_db(db)
    db.with_name(db.name + ".lock").write_text("pid", encoding="utf-8")
    assert run_all(root)["backup_v3"].status is Status.PASS


def build_api_tree(root):
    import json
    import test_checks_contract as tc
    (root / "api" / "conformance").mkdir(parents=True)
    files = {"api/openapi.yaml": tc.OPENAPI, "api/SYNC.md": tc.SYNC_MD,
             "api/conformance/README.md": tc.README,
             "api/conformance/server_scenarios.json": json.dumps(tc.SERVER, indent=2) + "\n",
             "api/conformance/client_cases.json": json.dumps(tc.CLIENT, indent=2) + "\n"}
    for rel, text in files.items():
        (root / rel).write_bytes(text.encode("utf-8"))


def run_cli(capsys, root, *args):
    code = verify.main(list(args), start=root)
    return code, capsys.readouterr().out


def test_cli_only_contract_m1_exits_2_when_live_incomplete(capsys, project):
    root, db = project
    build_db(db)
    db.with_name(db.name + ".lock").write_text("pid", encoding="utf-8")
    source_tree(root)
    build_api_tree(root)  # a valid contract and valid sources, so only the live schema is unverified
    code, out = run_cli(capsys, root, "--only", "contract,m1")
    assert code == core.EXIT_USAGE_OR_INCOMPLETE
    assert "[INCOMPLETE] m1.live_schema" in out and "[SKIP] m1.live_tables" in out


def test_cli_only_m1_exits_0_when_live_schema_verified(capsys, project):
    root, db = project
    build_db(db)
    build_db(db.with_name("tasknest.db.v3.bak"), version=3)
    source_tree(root)
    code, out = run_cli(capsys, root, "--only", "m1")
    assert code == core.EXIT_OK, out
    assert "[PASS] m1.live_schema" in out


def test_cli_exit_1_when_a_failure_accompanies_incomplete(capsys, project):
    root, db = project  # no source files: heuristic m1 source checks FAIL; live is missing -> INCOMPLETE
    code, out = run_cli(capsys, root, "--only", "m1")
    assert code == core.EXIT_FAIL and "[INCOMPLETE] m1.live_schema" in out
