import subprocess
import sys

import pytest

import fs_snapshot as fs
import gitro

pytestmark = pytest.mark.skipif(gitro.git_executable() is None, reason="git not available")

IDENT = ["-c", "user.name=T", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false"]


def git(root, *args):
    proc = subprocess.run(["git", *IDENT, *args], cwd=root, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout.strip()


@pytest.fixture
def repo(tmp_path):
    root = tmp_path / "r"
    root.mkdir()
    git(root, "init", "-q", "-b", "main")
    git(root, "config", "core.autocrlf", "false")  # keep fixture bytes identical on Windows
    (root / "a.txt").write_text("one\n", encoding="utf-8")
    (root / "sub").mkdir()
    (root / "sub" / "b.txt").write_text("two\n", encoding="utf-8")
    git(root, "add", ".")
    git(root, "commit", "-q", "-m", "init")
    (root / "a.txt").write_text("one\nchanged\n", encoding="utf-8")
    git(root, "add", "a.txt")                                              # staged change
    (root / "sub" / "b.txt").write_text("two\nmore\n", encoding="utf-8")   # unstaged
    (root / "new file.txt").write_text("x\n", encoding="utf-8")            # untracked
    return root


# --- every allowed command works -------------------------------------------------------------

def test_ls_files(repo):
    res = gitro.ls_files(repo)
    assert res.ok and gitro.parse_paths(res) == ["a.txt", "sub/b.txt"]


def test_staged_names(repo):
    res = gitro.staged_names(repo)
    assert res.ok and gitro.parse_paths(res) == ["a.txt"]


def test_status_includes_untracked_and_unstaged(repo):
    entries = {p: xy for xy, p in gitro.parse_status(gitro.status(repo))}
    assert entries == {"a.txt": "M ", "sub/b.txt": " M", "new file.txt": "??"}


def test_status_rename_origin_is_consumed(repo):
    git(repo, "mv", "sub/b.txt", "sub/c.txt")
    entries = gitro.parse_status(gitro.status(repo))
    assert ("RM", "sub/c.txt") in entries  # renamed in index, still modified in worktree
    assert all(p != "sub/b.txt" for _, p in entries)


def test_head_matches_git(repo):
    res = gitro.head(repo)
    assert res.ok and gitro.parse_single_line(res) == git(repo, "rev-parse", "HEAD")


def test_branch(repo):
    assert gitro.parse_single_line(gitro.current_branch(repo)) == "main"


def test_staged_diff(repo):
    res = gitro.staged_diff(repo)
    assert res.ok and "+changed" in res.text and "b.txt" not in res.text
    assert "changed" not in repr(res)  # raw output stays out of repr


def test_allowlist_is_exactly_six_commands():
    assert len(gitro.ALLOWED) == 6


# --- rejections ------------------------------------------------------------------------------

@pytest.mark.parametrize("args", [
    ["add", "."], ["commit", "-m", "x"], ["reset", "--hard"], ["checkout", "main"],
    ["clean", "-fd"], ["config", "user.name", "x"], ["push"], ["update-index", "--refresh"],
    ["gc"], ["stash"], ["tag", "x"], ["branch", "-D", "main"],
])
def test_mutation_commands_rejected(repo, args):
    with pytest.raises(gitro.GitRejected):
        gitro.run_git(repo, args)


@pytest.mark.parametrize("args", [
    [], ["log"], ["status"], ["ls-files"], ["ls-files", "-z", "--others"],
    ["status", "--porcelain=v1", "-z"], ["rev-parse", "--git-dir"], ["diff", "--cached"],
    ["--no-pager", "ls-files", "-z"], ["-c", "core.fsmonitor=true", "ls-files", "-z"],
    ["ls-files", "-z", ";", "rm"], ["ls-files", "-z "],
])
def test_unknown_or_altered_commands_rejected(repo, args):
    with pytest.raises(gitro.GitRejected):
        gitro.run_git(repo, args)


def test_rejection_does_not_start_a_process(repo, monkeypatch):
    def boom(*a, **k):
        raise AssertionError("subprocess started")
    monkeypatch.setattr(gitro.subprocess, "run", boom)
    with pytest.raises(gitro.GitRejected):
        gitro.run_git(repo, ["add", "."])


# --- safe process setup ----------------------------------------------------------------------

def test_invocation_flags(repo, monkeypatch):
    seen = {}

    class Done:
        returncode, stdout, stderr = 0, b"", b""

    def fake_run(argv, **kw):
        seen.update(argv=argv, **kw)
        return Done()
    monkeypatch.setattr(gitro.subprocess, "run", fake_run)
    monkeypatch.setenv("GIT_DIR", "/elsewhere")
    gitro.ls_files(repo)
    assert seen["shell"] is False
    assert seen["cwd"] == str(repo)
    assert seen["timeout"] == gitro.DEFAULT_TIMEOUT
    assert seen["env"]["GIT_OPTIONAL_LOCKS"] == "0"
    assert "GIT_DIR" not in seen["env"]
    argv = seen["argv"]
    assert argv[1:4] == ["--no-pager", "-c", "core.fsmonitor=false"]
    assert argv[4:] == ["ls-files", "-z"]


def test_all_commands_leave_repository_unchanged(repo):
    before = fs.snapshot(repo)
    assert any(p.startswith(".git/") for p in before)
    for fn in (gitro.ls_files, gitro.staged_names, gitro.status, gitro.head,
               gitro.current_branch, gitro.staged_diff):
        assert fn(repo).ok
    assert not fs.has_changes(fs.diff(before, fs.snapshot(repo)))


# --- failure handling ------------------------------------------------------------------------

def test_not_a_repository_is_a_safe_failure(tmp_path):
    empty = tmp_path / "plain"
    empty.mkdir()
    res = gitro.ls_files(empty)
    assert not res.ok and res.returncode not in (0, None)
    assert "exited" in res.describe()
    assert gitro.parse_paths(res) == [] and gitro.parse_status(res) == []


def test_missing_directory_raises_unavailable(tmp_path):
    with pytest.raises(gitro.GitUnavailable):
        gitro.ls_files(tmp_path / "nope")


def test_missing_git_raises_unavailable(repo, monkeypatch):
    monkeypatch.setattr(gitro, "git_executable", lambda: None)
    with pytest.raises(gitro.GitUnavailable):
        gitro.head(repo)


def test_timeout_is_reported_and_child_is_not_left_running(repo, tmp_path):
    marker = tmp_path / "survived.txt"
    script = tmp_path / "slow.py"
    script.write_text(
        "import time, pathlib\n"
        "time.sleep(30)\n"
        f"pathlib.Path({str(marker)!r}).write_text('x')\n", encoding="utf-8")
    res = gitro.ls_files(repo, timeout=0.5, _exe=[sys.executable, str(script)])
    assert res.timed_out and not res.ok and res.returncode is None
    assert "timed out" in res.describe()
    assert not marker.exists()


def test_launch_failure_is_reported(repo, tmp_path):
    res = gitro.ls_files(repo, _exe=[str(tmp_path / "no-such-binary")])
    assert not res.ok and res.error


def test_oversized_output_is_refused(repo, monkeypatch):
    monkeypatch.setattr(gitro, "MAX_OUTPUT_BYTES", 3)
    res = gitro.ls_files(repo)
    assert not res.ok and "size limit" in res.error
    assert res.stdout == b""


# --- secrets ---------------------------------------------------------------------------------

def test_redact_masks_secret_values():
    secret = "ghp_" + "a" * 24
    text = f"password=hunter2 token: abc {secret} https://u:pw@h/x Bearer abcdefghijkl AKIA{'A' * 16}"
    out = gitro.redact(text)
    for leaked in ("hunter2", secret, ":pw@", "abcdefghijkl", "AKIAAAAA"):
        assert leaked not in out
    assert out.count(gitro.REDACTED) >= 5


def test_stderr_is_redacted(repo, tmp_path):
    script = tmp_path / "noisy.py"
    script.write_text("import sys\nsys.stderr.write('fatal: password=s3cret\\n')\nsys.exit(3)\n",
                      encoding="utf-8")
    res = gitro.head(repo, _exe=[sys.executable, str(script)])
    assert res.returncode == 3 and "s3cret" not in res.stderr and "s3cret" not in res.describe()
