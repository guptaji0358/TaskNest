import json
import shutil
import subprocess
from pathlib import Path

import pytest

import checks_git as cg
import core
import gitro
import repo
import verify
from core import Status

pytestmark = pytest.mark.skipif(gitro.git_executable() is None, reason="git not available")

VERIFY_DIR = Path(__file__).resolve().parents[1]
IDENT = ["-c", "user.name=T", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false"]
# Built at run time so this source file itself never contains a scannable secret.
FAKE_TOKEN = "ghp_" + "Ab3dE6gH9jK2mN5pQ8sT"


def git(root, *args):
    proc = subprocess.run(["git", *IDENT, *args], cwd=root, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout.strip()


def write(root, rel, text="x\n"):
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


@pytest.fixture
def proj(tmp_path):
    """A temp git project with the root markers, a committed milestones.json and one commit."""
    root = tmp_path / "proj"
    root.mkdir()
    git(root, "init", "-q", "-b", "main")
    git(root, "config", "core.autocrlf", "false")
    write(root, "CMakeLists.txt", "project(x)\n")
    write(root, ".claude/CLAUDE.md", "# x\n")
    write(root, "tools/verify/milestones.json", (VERIFY_DIR / "milestones.json").read_text(encoding="utf-8"))
    write(root, "tools/verify/a.py", "a = 1\n")
    git(root, "add", ".")
    git(root, "commit", "-q", "-m", "init")
    return root


def run(root, *ids, milestone="M2.0", baseline=False):
    reg = core.Registry()
    cg.register(reg)
    plan = core.build_plan(reg, [f"git.{i}" for i in ids], False)
    results = core.run_plan(plan, repo.Ctx(root=root, milestone=milestone, baseline=baseline))
    return {r.id: r for r in results}


def one(root, id, **kw):
    return run(root, id, **kw)[id]


# --- registration and labelling ----------------------------------------------------------------

def test_kinds_are_labelled():
    reg = core.Registry()
    cg.register(reg)
    kinds = {s.id: s.kind for s in reg.specs()}
    assert kinds["staged_secrets"] == "heuristic"
    assert all(k == "deterministic" for i, k in kinds.items() if i != "staged_secrets")


# --- status facts ---------------------------------------------------------------------------------

def test_status_facts_reports_tracked_staged_modified_untracked(proj):
    write(proj, "tools/verify/a.py", "a = 2\n")
    git(proj, "add", "tools/verify/a.py")
    write(proj, "tools/verify/a.py", "a = 3\n")  # staged and modified again
    write(proj, "new file.txt")
    r = one(proj, "status_facts")
    assert r.status is Status.PASS
    for needle in ("tracked 4;", "staged 1;", "modified 1;", "untracked 1;", "new file.txt", "Facts only, no verdict"):
        assert needle in r.detail, r.detail
    assert "branch main (informational only)" in r.detail


def test_status_facts_is_deterministic(proj):
    for name in ("z.txt", "b.txt", "m.txt"):
        write(proj, name)
    assert one(proj, "status_facts").detail == one(proj, "status_facts").detail
    assert "b.txt, m.txt, z.txt" in one(proj, "status_facts").detail


def test_not_a_git_repository_is_incomplete(tmp_path):
    root = tmp_path / "nogit"
    root.mkdir()
    for id in ("status_facts", "forbidden_paths", "staged_secrets"):
        assert one(root, id).status is Status.INCOMPLETE


# --- forbidden paths -----------------------------------------------------------------------------------

@pytest.mark.parametrize("path,expected", [
    ("data/tasknest.db", True), ("a/b/x.sqlite3", True), ("out/bin/x.txt", True), ("app.exe", True),
    ("pkg/__pycache__/m.pyc", True), ("tasknest.db.v3.bak", True), ("scripts/src/main.cpp", False),
    ("tools/verify/out_of_scope.py", False), ("docs/db.md", False),
])
def test_forbidden_pattern_matching(path, expected):
    assert any(cg.path_matches(path, p) for p in cg.FORBIDDEN_TRACKED) is expected


def test_forbidden_paths_pass_on_a_clean_project(proj):
    r = one(proj, "forbidden_paths")
    assert r.status is Status.PASS and "untracked" in r.detail and "git-ignored" in r.detail
    assert "by path only" in r.detail


def test_forbidden_tracked_file_fails(proj):
    write(proj, "x.db", "binary")
    git(proj, "add", "-f", "x.db")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "1 tracked: x.db" in r.detail and "untracked" not in r.detail


@pytest.mark.parametrize("path", [
    "data/tasknest.db", "tasknest.db-wal", "tool.exe", "lib/x.dll", "build/a.o", "build/a.obj",
    "pkg/mod.pyc", "pkg/__pycache__/m.cpython-313.pyc", "out/bin/TaskNest.exe", "out/build/x.txt",
    "tasknest.db.v3.bak", "notes.sqlite3",
])
def test_forbidden_untracked_file_fails(proj, path):
    write(proj, path, "x")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "1 untracked" in r.detail and path in r.detail
    assert "tracked:" not in r.detail.replace("untracked:", "")


def test_tracked_and_untracked_are_reported_separately(proj):
    write(proj, "a.db")
    git(proj, "add", "-f", "a.db")
    write(proj, "b.exe")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "1 tracked: a.db" in r.detail and "1 untracked: b.exe" in r.detail


def test_ordinary_permitted_files_are_not_flagged(proj):
    for rel in ("scripts/src/main.cpp", "scripts/include/x.h", "scripts/main/Main.qml", "tests/tst_a.cpp",
                "api/openapi.yaml", "docs/database.md", "tools/verify/out_of_scope.py", "db.txt", "exe/readme.md",
                "outputs/notes.txt", "src/outside.py", "server/app/main.py", "CLOUD.md", ".claude/memory/x.md"):
        write(proj, rel, "ordinary\n")
    git(proj, "add", "scripts")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.PASS, r.detail


def test_toolkit_output_under_out_verify_is_not_flagged(proj):
    write(proj, "out/verify/baseline.json", "{}")
    write(proj, "out/verify/baseline.json.tmp", "{}")
    assert one(proj, "forbidden_paths").status is Status.PASS


def test_toolkit_output_is_flagged_only_when_it_independently_matches_a_rule(proj):
    write(proj, "out/verify/leftover.db", "x")
    write(proj, "out/verify/__pycache__/m.pyc", "x")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "out/verify/leftover.db" in r.detail and "m.pyc" in r.detail
    assert "baseline.json" not in r.detail


def test_tracked_out_verify_artifact_is_not_flagged_by_the_generic_out_rule(proj):
    write(proj, "out/verify/baseline.json", "{}")
    git(proj, "add", "-f", "out/verify/baseline.json")
    assert one(proj, "forbidden_paths").status is Status.PASS


def test_git_directory_is_never_inspected_as_a_project_artifact():
    for path in (".git/index.db", ".git/hooks/pre-commit.exe", ".git", ".git/objects/x.o"):
        assert cg.forbidden_pattern_hit(path) == ""
    assert cg.forbidden_pattern_hit("sub/.git/x.db") != ""  # only the repository's own .git is exempt
    assert cg.forbidden_pattern_hit("out/verify/baseline.json") == ""
    assert cg.forbidden_pattern_hit("out/bin/x") == "out/**"


def test_path_matches_rules():
    assert cg.path_matches("tools/verify/x.py", "tools/verify/**")
    assert not cg.path_matches("tools/verifyx/x.py", "tools/verify/**")
    assert cg.path_matches("CLOUD.md", "CLOUD.md") and not cg.path_matches("a/CLOUD.md", "CLOUD.md")


# --- staged secret scan (heuristic) -------------------------------------------------------------------

def test_staged_secret_is_reported_by_file_line_and_pattern_only(proj, capsys):
    write(proj, "conf.txt", f"line one\nline two\nkey = {FAKE_TOKEN}\n")
    git(proj, "add", "conf.txt")
    r = one(proj, "staged_secrets")
    assert r.status is Status.FAIL and r.kind == "heuristic"
    assert "conf.txt:3 [github_token]" in r.detail
    assert FAKE_TOKEN not in r.detail and FAKE_TOKEN[4:] not in r.detail
    code = verify.main(["--only", "git.staged_secrets", "--json"], start=proj)
    out = capsys.readouterr().out
    assert code == core.EXIT_FAIL and FAKE_TOKEN not in out and FAKE_TOKEN[4:] not in out
    assert '"kind": "heuristic"' in out
    code = verify.main(["--only", "git.staged_secrets"], start=proj)
    human = capsys.readouterr().out
    assert FAKE_TOKEN not in human and "(heuristic)" in human and "values not shown" in human


def test_unstaged_and_removed_secrets_are_not_reported(proj):
    write(proj, "conf.txt", f"key = {FAKE_TOKEN}\n")  # not staged
    assert one(proj, "staged_secrets").status is Status.PASS
    git(proj, "add", "conf.txt")
    git(proj, "commit", "-q", "-m", "c")
    write(proj, "conf.txt", "clean\n")  # staged removal of the secret line
    git(proj, "add", "conf.txt")
    assert one(proj, "staged_secrets").status is Status.PASS  # only additions are examined


def test_secret_findings_are_sorted_and_deterministic(proj):
    write(proj, "b.txt", f"x\n{FAKE_TOKEN}\n")
    write(proj, "a.txt", "-----BEGIN RSA PRIVATE KEY-----\n")
    git(proj, "add", ".")
    first = one(proj, "staged_secrets").detail
    assert first == one(proj, "staged_secrets").detail
    assert first.index("a.txt:1 [private_key_block]") < first.index("b.txt:2 [github_token]")


@pytest.mark.parametrize("text,name", [
    ("-----BEGIN PRIVATE KEY-----", "private_key_block"),
    ("t = " + "ghp_" + "A" * 20, "github_token"),
    ("id = " + "AKIA" + "ABCDEFGH23456789", "aws_access_key_id"),
    ("Authorization: Bearer " + "abcdefgh12345678xyz", "bearer_token"),
    ("url = https://user:" + "hunter22xx@host/x", "url_credentials"),
    ('password = "' + "Zq8vL2mP9xKd4Wr7" + '"', "credential_assignment"),
])
def test_each_pattern_is_detected_and_value_free(text, name):
    diff = f"+++ b/f.txt\n@@ -0,0 +7,1 @@\n+{text}\n"
    assert cg.scan_diff(diff) == [("f.txt", 7, name)]


@pytest.mark.parametrize("text", [
    'password = "<your password>"', "token = ${TOKEN_FROM_ENV}", "api_key = example-key-0000000", "password = ''",
    "the word password appears here", "Bearer short",
])
def test_placeholders_and_prose_are_not_flagged(text):
    assert cg.scan_diff(f"+++ b/f.txt\n@@ -0,0 +1,1 @@\n+{text}\n") == []


def test_deleted_files_and_context_lines_are_skipped():
    diff = (f"--- a/f.txt\n+++ /dev/null\n@@ -1,1 +0,0 @@\n-key = {FAKE_TOKEN}\n"
            f"--- a/g.txt\n+++ b/g.txt\n@@ -1,1 +1,1 @@\n-old {FAKE_TOKEN}\n")
    assert cg.scan_diff(diff) == []


# --- git-ignored files (filesystem scan by path) -----------------------------------------------------

IGNORE = "*.db\n*.exe\n*.o\n*.pyc\n*.bak\n*.log\nbuild/\nout/\n"


def ignore_setup(proj):
    write(proj, ".gitignore", IGNORE)
    git(proj, "add", ".gitignore")
    git(proj, "commit", "-q", "-m", "ignore")


@pytest.mark.parametrize("path", [
    "data/ignored.db", "tool.exe", "lib/mod.o", "pkg/mod.pyc", "old.bak",
])
def test_ignored_forbidden_files_are_found_and_labelled_ignored(proj, path):
    ignore_setup(proj)
    write(proj, path, "x")
    assert path not in git(proj, "status", "--porcelain", "--untracked-files=all")  # really git-ignored
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and f"1 ignored: {path}" in r.detail
    assert "tracked:" not in r.detail.replace("untracked:", "")


def test_ignored_build_output_directory_is_flagged_without_listing_its_contents(proj):
    ignore_setup(proj)
    for name in ("a.txt", "deep/b.txt"):
        write(proj, f"build/{name}", "x")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "1 ignored: build/" in r.detail and "a.txt" not in r.detail


def test_ignored_pycache_directory_is_flagged(proj):
    write(proj, ".gitignore", "__pycache__/\n")
    git(proj, "add", ".gitignore")
    write(proj, "pkg/__pycache__/m.cpython-313.pyc", "x")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "ignored: pkg/__pycache__/" in r.detail


def test_tracked_untracked_and_ignored_are_distinguished(proj):
    ignore_setup(proj)
    write(proj, "t.db")
    git(proj, "add", "-f", "t.db")
    write(proj, "u.exe.txt", "ok")          # untracked, permitted
    write(proj, "u.dll")                    # untracked, forbidden
    write(proj, "i.bak")                    # ignored, forbidden
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL
    assert "1 tracked: t.db" in r.detail and "1 untracked: u.dll" in r.detail and "1 ignored: i.bak" in r.detail


def test_permitted_ignored_files_are_not_flagged(proj):
    ignore_setup(proj)
    for rel in ("run.log", "notes/trace.log"):  # ignored but not forbidden artifacts
        write(proj, rel, "log")
    assert one(proj, "forbidden_paths").status is Status.PASS


def test_out_directory_is_not_entered_except_out_verify(proj):
    ignore_setup(proj)
    for rel in ("out/bin/data/tasknest.db", "out/build/x.o", "out/bin/TaskNest.exe", "out/server/venv/a.pyc"):
        write(proj, rel, "generated")  # the designated git-ignored output area
    assert one(proj, "forbidden_paths").status is Status.PASS


def test_out_verify_exception_applies_to_ignored_files_too(proj):
    ignore_setup(proj)
    write(proj, "out/verify/baseline.json", "{}")
    write(proj, "out/verify/baseline.json.tmp", "{}")
    assert one(proj, "forbidden_paths").status is Status.PASS
    write(proj, "out/verify/leftover.db", "x")
    write(proj, "out/verify/cache/m.pyc", "x")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "2 ignored: out/verify/cache/m.pyc, out/verify/leftover.db" in r.detail
    assert "baseline.json" not in r.detail


def test_dot_git_directory_is_excluded_but_nested_git_is_not(proj):
    write(proj, ".git/info/stray.db", "x")  # inside the repository's own .git/: never a project artifact
    assert one(proj, "forbidden_paths").status is Status.PASS
    write(proj, "vendor/.git/x.db", "x")
    assert "vendor/.git/x.db" in one(proj, "forbidden_paths").detail


def test_external_trees_are_skipped(proj):
    ignore_setup(proj)
    write(proj, "node_modules/pkg/x.db", "x")
    write(proj, ".venv/lib/m.pyc", "x")
    assert one(proj, "forbidden_paths").status is Status.PASS


def make_symlink(link, target):
    try:
        link.symlink_to(target, target_is_directory=target.is_dir())
    except (OSError, NotImplementedError):
        pytest.skip("symlinks are not available here")


def test_symlinked_directory_outside_the_repository_is_not_followed(proj, tmp_path):
    ignore_setup(proj)
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "external.db").write_bytes(b"x")
    make_symlink(proj / "linked", outside)
    r = one(proj, "forbidden_paths")
    assert r.status is Status.PASS, r.detail  # external.db was never reached through the link


def test_symlink_is_judged_by_its_own_name_and_target_is_not_resolved(proj, tmp_path):
    ignore_setup(proj)
    outside = tmp_path / "t.txt"
    outside.write_text("x", encoding="utf-8")
    make_symlink(proj / "alias.db", outside)  # a link NAMED like a database
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "alias.db" in r.detail
    (proj / "alias.db").unlink()
    make_symlink(proj / "harmless.txt", tmp_path / "missing.db")  # dangling link whose target name is forbidden
    assert one(proj, "forbidden_paths").status is Status.PASS


def test_scan_reads_no_file_contents(proj, monkeypatch):
    ignore_setup(proj)
    write(proj, "secret.bak", "contents that must not be read")
    opened = []
    real_open = open

    def spy(file, *a, **kw):
        opened.append(str(file))
        return real_open(file, *a, **kw)

    monkeypatch.setattr("builtins.open", spy)
    monkeypatch.setattr(cg, "file_sha256", lambda *a, **k: (_ for _ in ()).throw(AssertionError("hashed")))
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "secret.bak" in r.detail
    assert not [f for f in opened if "secret.bak" in f]


def test_scan_stops_at_the_entry_limit_and_is_never_a_false_pass(proj, monkeypatch):
    ignore_setup(proj)
    for i in range(30):
        write(proj, f"many/f{i:02d}.txt", "x")
    monkeypatch.setattr(cg, "MAX_SCAN_ENTRIES", 10)
    r = one(proj, "forbidden_paths")
    assert r.status is Status.INCOMPLETE and "not fully inspected" in r.detail


def test_truncated_scan_with_findings_still_fails_and_says_so(proj, monkeypatch):
    ignore_setup(proj)
    write(proj, "a.bak")
    for i in range(30):
        write(proj, f"z/f{i:02d}.txt", "x")
    monkeypatch.setattr(cg, "MAX_SCAN_ENTRIES", 12)
    r = one(proj, "forbidden_paths")
    assert r.status is Status.FAIL and "a.bak" in r.detail and "scan stopped" in r.detail


def test_windows_junction_to_an_external_directory_is_not_followed(proj, tmp_path):
    import os
    if os.name != "nt":
        pytest.skip("junctions are Windows-only")
    ignore_setup(proj)
    outside = tmp_path / "outside_j"
    outside.mkdir()
    (outside / "external.db").write_bytes(b"x")
    proc = subprocess.run(["cmd", "/c", "mklink", "/J", str(proj / "jlink"), str(outside)],
                          capture_output=True, text=True, timeout=30)
    if proc.returncode != 0:
        pytest.skip("cannot create a junction here")
    r = one(proj, "forbidden_paths")
    assert r.status is Status.PASS, r.detail  # external.db was never reached through the junction


def test_scan_ignored_unit_respects_max_entries(proj):
    for i in range(30):
        write(proj, f"many/f{i:02d}.txt", "x")
    findings, scanned, truncated = cg.scan_ignored(proj, set(), set(), max_entries=10)
    assert truncated and scanned == 11
    findings, scanned, truncated = cg.scan_ignored(proj, set(), set())
    assert not truncated and findings == []
