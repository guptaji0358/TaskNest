"""Write isolation: the toolkit may write only under out/verify/ (or system temp).

The CLI runs against a complete temporary copy of the toolkit inside a throw-away
git repository, so `.git/` and `out/` can be snapshotted without touching the real
repository's metadata.
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

import core
import fs_snapshot as fs

TOOL_FILES = ("verify.py", "core.py", "repo.py", "gitro.py", "sqlro.py", "checks_contract.py", "checks_m1.py", "checks_git.py", "checks_memory.py", "checks_server.py", "milestones.json")
VERIFY_DIR = Path(__file__).resolve().parents[1]


@pytest.fixture
def temp_project(tmp_path):
    """Temp git repo with the root markers, a copy of the toolkit, and files in out/ and .git/."""
    if shutil.which("git") is None:
        pytest.skip("git not available")
    root = tmp_path / "proj"
    (root / ".claude").mkdir(parents=True)
    (root / "CMakeLists.txt").write_text("project(x)\n", encoding="utf-8")
    (root / ".claude" / "CLAUDE.md").write_text("# x\n", encoding="utf-8")
    tool_dir = root / "tools" / "verify"
    tool_dir.mkdir(parents=True)
    for name in TOOL_FILES:
        shutil.copy2(VERIFY_DIR / name, tool_dir / name)
    (root / "out" / "bin").mkdir(parents=True)
    (root / "out" / "bin" / "keep.txt").write_text("live data\n", encoding="utf-8")
    (root / "untracked.txt").write_text("u\n", encoding="utf-8")
    proc = subprocess.run(["git", "init", "-q", str(root)], capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, proc.stderr
    return root


def run_cli(root, *args):
    return subprocess.run([sys.executable, "-B", str(root / "tools" / "verify" / "verify.py"), *args],
                          capture_output=True, text=True, timeout=60, cwd=root,
                          env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})


# --- the toolkit itself writes nothing outside the permitted locations ---------------------

def test_cli_changes_nothing_in_a_complete_project_copy(temp_project):
    before = fs.snapshot(temp_project)
    assert any(p.startswith(".git/") for p in before)  # .git really is being watched
    assert "out/bin/keep.txt" in before and "untracked.txt" in before
    for args in ([], ["--json"], ["--list"], ["--only", "m1"], ["--only", "git"], ["--baseline"], ["--full"]):
        proc = run_cli(temp_project, *args)
        assert proc.returncode in (core.EXIT_OK, core.EXIT_FAIL, core.EXIT_USAGE_OR_INCOMPLETE), proc.stderr
    results = json.loads(run_cli(temp_project, "--json", "--full").stdout)["results"]
    by_id = {f"{r['group']}.{r['id']}": r["status"] for r in results}
    assert by_id["contract.files_present"] == "FAIL"  # the temp project has no api/ directory
    assert by_id["m1.live_user_version"] == "SKIP"    # and no live database
    assert by_id["git.milestone_scope"] == "INCOMPLETE"  # allowed paths are only PROPOSED
    assert by_id["memory.verify"] == "INCOMPLETE"        # no .claude/scripts/verify_memory.py in the temp project
    assert by_id["server.check"] == "INCOMPLETE"         # --full ran it; no server/check.py in the temp project
    assert (temp_project / "tools" / "verify" / "milestones.json").is_file()
    result = fs.diff(before, fs.snapshot(temp_project))
    assert not fs.has_changes(result), result
    assert not (temp_project / "out" / "verify").exists()  # nothing generated in this phase


# --- the detector itself: unexpected writes are caught, permitted ones are not -------------

def changed(root, mutate):
    before = fs.snapshot(root)
    mutate(root)
    return fs.diff(before, fs.snapshot(root))


def test_detects_new_file_in_git_dir(temp_project):
    res = changed(temp_project, lambda r: (r / ".git" / "evil").write_text("x", encoding="utf-8"))
    assert res["created"] == [".git/evil"]


def test_detects_modified_file_in_git_dir(temp_project):
    head = temp_project / ".git" / "HEAD"
    res = changed(temp_project, lambda r: head.write_text("ref: refs/heads/other\n", encoding="utf-8"))
    assert res["modified"] == [".git/HEAD"]


def test_detects_deleted_file_in_git_dir(temp_project):
    res = changed(temp_project, lambda r: (r / ".git" / "HEAD").unlink())
    assert res["deleted"] == [".git/HEAD"]


def test_detects_changes_in_out_outside_verify(temp_project):
    def mutate(r):
        (r / "out" / "bin" / "keep.txt").write_text("tampered\n", encoding="utf-8")
        (r / "out" / "bin" / "new.db").write_text("x", encoding="utf-8")
        (r / "out" / "build").mkdir()
    res = changed(temp_project, mutate)
    assert res["modified"] == ["out/bin/keep.txt"]
    assert res["created"] == ["out/bin/new.db", "out/build"]


def test_detects_deleted_out_file(temp_project):
    res = changed(temp_project, lambda r: (r / "out" / "bin" / "keep.txt").unlink())
    assert res["deleted"] == ["out/bin/keep.txt"]


def test_detects_untracked_and_pycache_files(temp_project):
    def mutate(r):
        (r / "stray.txt").write_text("x", encoding="utf-8")
        (r / "untracked.txt").write_text("changed\n", encoding="utf-8")
        (r / "tools" / "verify" / "__pycache__").mkdir()
        (r / "tools" / "verify" / "__pycache__" / "core.cpython-313.pyc").write_bytes(b"\0")
    res = changed(temp_project, mutate)
    assert res["created"] == ["stray.txt", "tools/verify/__pycache__",
                              "tools/verify/__pycache__/core.cpython-313.pyc"]
    assert res["modified"] == ["untracked.txt"]


def test_same_size_content_change_is_detected(temp_project):
    target = temp_project / "untracked.txt"
    res = changed(temp_project, lambda r: target.write_text("v\n", encoding="utf-8"))  # same length as "u\n"
    assert res["modified"] == ["untracked.txt"]


def test_writes_under_out_verify_are_permitted(temp_project):
    def mutate(r):
        (r / "out" / "verify" / "pycache").mkdir(parents=True)
        (r / "out" / "verify" / "pycache" / "x.pyc").write_bytes(b"\0")
        (r / "out" / "verify" / "baseline.json").write_text("{}", encoding="utf-8")
    assert not fs.has_changes(changed(temp_project, mutate))


def test_out_verify_prefix_does_not_cover_lookalike_paths(temp_project):
    def mutate(r):
        (r / "out" / "verify-evil").mkdir()
        (r / "out" / "verifyx.txt").write_text("x", encoding="utf-8")
    assert changed(temp_project, mutate)["created"] == ["out/verify-evil", "out/verifyx.txt"]


def test_no_change_means_no_report(temp_project):
    assert not fs.has_changes(changed(temp_project, lambda r: None))


def test_diff_helper_unit():
    before = {"a": "file:1:x", "b": "dir", "out/verify/z": "file:1:y"}
    after = {"a": "file:1:z", "c": "dir", "out/verify/z": "file:2:q", "out/verify/n": "dir"}
    assert fs.diff(before, after) == {"created": ["c"], "deleted": ["b"], "modified": ["a"]}


# --- owner-approved exception: out/server/ only for the server checker (Step 3.5) -----------

def test_out_server_is_not_permitted_by_default(temp_project):
    def mutate(r):
        (r / "out" / "server" / "pycache").mkdir(parents=True)
        (r / "out" / "server" / "pycache" / "x.pyc").write_bytes(b"0")
    res = changed(temp_project, mutate)
    assert res["created"] == ["out/server", "out/server/pycache", "out/server/pycache/x.pyc"]


def test_server_checker_scope_permits_out_server_but_nothing_else(temp_project):
    before = fs.snapshot(temp_project)
    (temp_project / "out" / "server" / ".pytest_cache").mkdir(parents=True)
    (temp_project / "out" / "server-evil").mkdir()
    (temp_project / "out" / "bin" / "keep.txt").write_text("tampered\n", encoding="utf-8")
    (temp_project / "server").mkdir()
    (temp_project / "server" / "stray.txt").write_text("x", encoding="utf-8")
    res = fs.diff(before, fs.snapshot(temp_project), fs.SERVER_CHECKER_PREFIXES)
    assert res["created"] == ["out/server-evil", "server", "server/stray.txt"]
    assert res["modified"] == ["out/bin/keep.txt"]


def test_server_adapter_run_writes_only_inside_out_server(temp_project):
    import checks_server as cs
    import repo
    (temp_project / "server").mkdir()
    (temp_project / "server" / "check.py").write_text(
        "from pathlib import Path\n"
        "o = Path(__file__).resolve().parent.parent / 'out' / 'server' / 'pycache'\n"
        "o.mkdir(parents=True, exist_ok=True); (o / 'a.pyc').write_bytes(b'0')\n", encoding="utf-8")
    before = fs.snapshot(temp_project)
    assert cs.check_server(repo.Ctx(root=temp_project)).status is core.Status.PASS
    after = fs.snapshot(temp_project)
    assert "out/server/pycache/a.pyc" in after
    assert not fs.has_changes(fs.diff(before, after, fs.SERVER_CHECKER_PREFIXES))
    assert not (temp_project / "out" / "verify" / "memory").exists()
