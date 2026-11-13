import hashlib
import sqlite3
import sys

import pytest

import checks_memory as cm
import fs_snapshot as fs
import repo
from core import Status


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def setup(root, script_body, db_bytes=b"DBDATA"):
    (root / ".claude" / "memory").mkdir(parents=True, exist_ok=True)
    (root / ".claude" / "scripts").mkdir(parents=True, exist_ok=True)
    (root / ".claude" / "scripts" / "verify_memory.py").write_text(script_body, encoding="utf-8")
    db = root / ".claude" / "memory" / "memory.db"
    db.write_bytes(db_bytes)
    return db


def run(root):
    return cm.check_memory(repo.Ctx(root=root))


def test_uses_copy_via_tnmem_db_and_leaves_original_identical(fake_repo):
    db = setup(fake_repo, "import os,sys,pathlib\n"
               "p=pathlib.Path(os.environ['TNMEM_DB'])\n"
               "assert p.read_bytes()==b'DBDATA'\n"
               "assert 'out' in p.parts and 'verify' in p.parts and p != pathlib.Path(sys.argv[0])\n"
               "p.write_bytes(b'MUTATED')\n"  # mutate the copy
               "print('ok'); sys.exit(0)\n")
    before = sha(db)
    r = run(fake_repo)
    assert r.status is Status.PASS, r.detail
    assert sha(db) == before
    assert not (fake_repo / "out" / "verify" / "memory").exists()  # copy cleaned up


def test_env_hygiene(fake_repo, monkeypatch):
    monkeypatch.setenv("TNMEM_DB", "/live/path")
    monkeypatch.setenv("TASKNEST_X", "1")
    script = chr(10).join([
        'import os, sys',
        "db = os.environ['TNMEM_DB'].replace(chr(92), '/')",
        "log = os.environ['TNMEM_LOG'].replace(chr(92), '/')",
        "ok = db.endswith('out/verify/memory/memory.db') and 'out/verify' in log",
        "ok = ok and not any(k.startswith('TASKNEST_') for k in os.environ)",
        'sys.exit(0 if ok else 1)', ''])
    setup(fake_repo, script)
    assert run(fake_repo).status is Status.PASS


@pytest.mark.parametrize("suffix", ["-wal", "-shm", "-journal"])
def test_sidecar_skips_without_copy_or_run(fake_repo, suffix):
    db = setup(fake_repo, "raise SystemExit(1)\n")
    (db.parent / ("memory.db" + suffix)).write_bytes(b"x")
    r = run(fake_repo)
    assert r.status is Status.SKIP and suffix in r.detail
    assert not (fake_repo / "out").exists()


def test_missing_db_skips_and_missing_script_incomplete(fake_repo):
    (fake_repo / ".claude" / "scripts").mkdir(parents=True)
    assert run(fake_repo).status is Status.INCOMPLETE
    (fake_repo / ".claude" / "scripts" / "verify_memory.py").write_text("pass\n", encoding="utf-8")
    assert run(fake_repo).status is Status.SKIP


def test_copy_failure_is_incomplete_and_original_untouched(fake_repo, monkeypatch):
    db = setup(fake_repo, "pass\n")
    before = sha(db)
    def boom(*a, **k):
        raise PermissionError("denied")
    monkeypatch.setattr(cm.shutil, "copyfileobj", boom)
    r = run(fake_repo)
    assert r.status is Status.INCOMPLETE and "could not copy" in r.detail
    assert sha(db) == before


def test_exit_codes_and_output_rules(fake_repo):
    setup(fake_repo, "import sys\nprint('PROBLEM: password=hunter2hunter2 ' + 'z'*9000)\nsys.exit(1)\n")
    r = run(fake_repo)
    assert r.status is Status.FAIL
    assert "hunter2" not in r.detail and "[output truncated]" in r.detail
    setup(fake_repo, "import sys\nprint('stats: ok')\nsys.exit(0)\n")
    r = run(fake_repo)
    assert r.status is Status.PASS and "stats" not in r.detail
    setup(fake_repo, "import sys; sys.exit(5)\n")
    assert run(fake_repo).status is Status.FAIL


def test_launch_failure_and_timeout_incomplete(fake_repo, monkeypatch):
    db = setup(fake_repo, "pass\n")
    before = sha(db)
    def boom(*a, **k):
        raise OSError("x")
    monkeypatch.setattr(cm.subprocess, "run", boom)
    assert run(fake_repo).status is Status.INCOMPLETE
    def slow(*a, **k):
        raise cm.subprocess.TimeoutExpired(a[0], 1)
    monkeypatch.setattr(cm.subprocess, "run", slow)
    assert run(fake_repo).status is Status.INCOMPLETE
    assert sha(db) == before and not (fake_repo / "out" / "verify" / "memory").exists()


def test_original_changed_during_run_is_fail(fake_repo):
    setup(fake_repo, "import pathlib,sys\n"
          f"pathlib.Path(r'{fake_repo / '.claude' / 'memory' / 'memory.db'}').write_bytes(b'CHANGED')\n")
    r = run(fake_repo)
    assert r.status is Status.FAIL and "changed during verification" in r.detail


def test_wal_mode_real_sqlite_copy_works_with_real_verifier_shape(fake_repo):
    db = setup(fake_repo, "import os,sqlite3,sys\n"
               "c=sqlite3.connect(os.environ['TNMEM_DB']); c.execute('select count(*) from t'); c.close()\n")
    db.unlink()
    con = sqlite3.connect(db); con.execute("create table t(x)"); con.commit(); con.close()
    before = sha(db)
    assert run(fake_repo).status is Status.PASS
    assert sha(db) == before


def test_writes_only_under_out_verify(fake_repo):
    setup(fake_repo, "import sys; print('p'); sys.exit(1)\n")
    before = fs.snapshot(fake_repo)
    run(fake_repo)
    res = fs.diff(before, fs.snapshot(fake_repo))
    assert res == {"created": ["out"], "deleted": [], "modified": []}  # only the out/ parent of out/verify/
    assert not (fake_repo / "out" / "verify" / "memory").exists()
