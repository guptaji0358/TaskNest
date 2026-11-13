import sys

import pytest

import checks_server as cs
from core import Registry, Status, run_plan, build_plan


def make_script(root, body):
    d = root / "server"
    d.mkdir(exist_ok=True)
    (d / "check.py").write_text(body, encoding="utf-8")


def ctx_for(root):
    import repo
    return repo.Ctx(root=root)


@pytest.mark.parametrize("code,status", [(0, Status.PASS), (1, Status.FAIL), (2, Status.INCOMPLETE),
                                         (3, Status.FAIL), (7, Status.FAIL), (255, Status.FAIL)])
def test_exit_code_mapping(fake_repo, code, status):
    make_script(fake_repo, f"import sys; print('some output'); sys.exit({code})\n")
    r = cs.check_server(ctx_for(fake_repo))
    assert r.status is status
    if status is Status.PASS:
        assert "some output" not in r.detail
    else:
        assert "some output" in r.detail
    if code == 3:
        assert "unexpected code 3" in r.detail


def test_missing_script_is_incomplete(fake_repo):
    r = cs.check_server(ctx_for(fake_repo))
    assert r.status is Status.INCOMPLETE and "not found" in r.detail


def test_launch_failure_is_incomplete(fake_repo, monkeypatch):
    make_script(fake_repo, "pass\n")
    def boom(*a, **k):
        raise FileNotFoundError("nope")
    monkeypatch.setattr(cs.subprocess, "run", boom)
    r = cs.check_server(ctx_for(fake_repo))
    assert r.status is Status.INCOMPLETE and "could not be launched" in r.detail


def test_timeout_is_incomplete(fake_repo, monkeypatch):
    make_script(fake_repo, "pass\n")
    def slow(*a, **k):
        raise cs.subprocess.TimeoutExpired(a[0], 1, output=b"partial")
    monkeypatch.setattr(cs.subprocess, "run", slow)
    r = cs.check_server(ctx_for(fake_repo))
    assert r.status is Status.INCOMPLETE and "timed out" in r.detail


def test_exact_command_cwd_and_no_shell(fake_repo, monkeypatch):
    make_script(fake_repo, "pass\n")
    seen = {}
    class P:
        returncode = 0
        stdout = b""
    def fake(cmd, **kw):
        seen["cmd"], seen["kw"] = cmd, kw
        return P()
    monkeypatch.setattr(cs.subprocess, "run", fake)
    cs.check_server(ctx_for(fake_repo))
    assert seen["cmd"] == [sys.executable, "-B", str(fake_repo / "server" / "check.py")]
    assert seen["kw"]["cwd"] == str(fake_repo) and seen["kw"]["shell"] is False


def test_environment(fake_repo, monkeypatch):
    monkeypatch.setenv("TASKNEST_SECRET_X", "v")
    monkeypatch.setenv("tasknest_lower", "v")
    monkeypatch.setenv("KEEP_ME", "1")
    make_script(fake_repo, "import os,sys\n"
                "bad=[k for k in os.environ if k.upper().startswith('TASKNEST_')]\n"
                "ok=os.environ.get('PYTHONDONTWRITEBYTECODE')=='1' and os.environ.get('KEEP_ME')=='1'\n"
                "pre=os.environ.get('PYTHONPYCACHEPREFIX','')\n"
                f"ok=ok and pre==r'{fake_repo / 'out' / 'verify' / 'pycache'}'\n"
                "print(bad, ok); sys.exit(0 if (not bad and ok) else 1)\n")
    assert cs.check_server(ctx_for(fake_repo)).status is Status.PASS


def test_truncation_and_redaction(fake_repo):
    make_script(fake_repo, "import sys\nprint('password=hunter2hunter2 ' + 'x'*10000)\nsys.exit(1)\n")
    r = cs.check_server(ctx_for(fake_repo))
    assert "hunter2" not in r.detail and "[REDACTED]" in r.detail
    assert "[output truncated]" in r.detail
    assert len(r.detail) < cs.MAX_OUTPUT_CHARS + 200


def test_secret_split_by_truncation_not_leaked(fake_repo):
    pad = "y" * (cs.MAX_OUTPUT_CHARS - 20)
    make_script(fake_repo, f"import sys\nprint('{pad} token=SUPERSECRETVALUE123')\nsys.exit(2)\n")
    r = cs.check_server(ctx_for(fake_repo))
    assert "SUPERSECRET" not in r.detail and "SUPERSE" not in r.detail


def test_skip_without_full_and_runs_with_full(fake_repo):
    make_script(fake_repo, "import sys; sys.exit(0)\n")
    reg = Registry()
    cs.register(reg)
    ctx = ctx_for(fake_repo)
    res = run_plan(build_plan(reg, [], False), ctx)
    assert [r.status for r in res] == [Status.SKIP]
    res = run_plan(build_plan(reg, [], True), ctx)
    assert [r.status for r in res] == [Status.PASS]


def test_nothing_written_outside_out(fake_repo):
    import fs_snapshot as fs
    make_script(fake_repo, "import sys; print('x'); sys.exit(1)\n")
    before = fs.snapshot(fake_repo)
    cs.check_server(ctx_for(fake_repo))
    assert not fs.has_changes(fs.diff(before, fs.snapshot(fake_repo)))
