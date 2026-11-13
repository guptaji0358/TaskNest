import json
import shutil

import pytest

import checks_git as cg
import core
import fs_snapshot as fs
import gitro
import verify
from core import Status
from test_checks_git import FAKE_TOKEN, git, one, proj, run, write  # noqa: F401  (proj is a fixture)

pytestmark = pytest.mark.skipif(gitro.git_executable() is None, reason="git not available")


def baseline_file(root):
    return root / "out" / "verify" / "baseline.json"


def cli(capsys, root, *args):
    code = verify.main(list(args), start=root)
    captured = capsys.readouterr()
    return code, captured.out, captured.err


# --- baseline save -------------------------------------------------------------------------------

def test_ordinary_verification_never_creates_a_baseline(proj, capsys):
    write(proj, "scratch.txt")
    for args in ([], ["--json"], ["--only", "git"], ["--baseline"]):
        cli(capsys, proj, *args)
    assert not (proj / "out").exists()


def test_baseline_save_records_head_paths_and_content_hashes(proj, capsys):
    write(proj, "tools/verify/a.py", "a = 2\n")
    write(proj, "new.txt", "hello\n")
    code, out, _ = cli(capsys, proj, "--baseline-save")
    assert code == core.EXIT_OK and "baseline saved" in out
    data = json.loads(baseline_file(proj).read_text(encoding="utf-8"))
    assert data["format"] == cg.BASELINE_FORMAT and data["head"] == git(proj, "rev-parse", "HEAD")
    assert sorted(data["entries"]) == ["new.txt", "tools/verify/a.py"]
    import hashlib
    assert data["entries"]["new.txt"]["sha256"] == hashlib.sha256(b"hello\n").hexdigest()
    assert "hello" not in baseline_file(proj).read_text(encoding="utf-8")  # hashes, never content


def test_baseline_save_is_deterministic_and_excludes_tool_output(proj, capsys):
    write(proj, "b.txt")
    write(proj, "a.txt")
    cli(capsys, proj, "--baseline-save")
    first = baseline_file(proj).read_bytes()
    code, _, err = cli(capsys, proj, "--baseline-save", "--force")  # baseline.json itself is untracked now
    assert code == core.EXIT_OK and baseline_file(proj).read_bytes() == first
    assert "out/verify" not in first.decode("utf-8")


def test_baseline_save_refuses_to_overwrite_without_force(proj, capsys):
    cli(capsys, proj, "--baseline-save")
    before = baseline_file(proj).read_bytes()
    write(proj, "later.txt")
    code, _, err = cli(capsys, proj, "--baseline-save")
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "--force" in err
    assert baseline_file(proj).read_bytes() == before
    code, out, _ = cli(capsys, proj, "--baseline-save", "--force")
    assert code == core.EXIT_OK and "later.txt" in baseline_file(proj).read_text(encoding="utf-8")


@pytest.mark.parametrize("args", [["--force"], ["--baseline-save", "--baseline"], ["--baseline-save", "--only", "git"],
                                  ["--baseline-save", "--json"], ["--baseline-save", "--list"]])
def test_baseline_flag_misuse_is_a_usage_error_and_writes_nothing(proj, capsys, args):
    code, _, err = cli(capsys, proj, *args)
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and err
    assert not (proj / "out").exists()


def test_baseline_save_outside_a_git_repo_fails_and_writes_nothing(tmp_path, capsys):
    root = tmp_path / "nogit"
    write(root, "CMakeLists.txt", "x\n")
    write(root, ".claude/CLAUDE.md", "x\n")
    code, _, err = cli(capsys, root, "--baseline-save")
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "git" in err
    assert not (root / "out").exists()


# --- baseline link isolation (symlinks / junctions) -------------------------------------------------

def make_link(kind, link, target, is_dir=True):
    """Create a symlink or Windows junction; skip explicitly (never pass silently) if unsupported."""
    import os
    import subprocess
    if kind == "junction":
        if os.name != "nt":
            pytest.skip("junctions are Windows-only")
        proc = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                              capture_output=True, text=True, timeout=30)
        if proc.returncode != 0:
            pytest.skip("cannot create a junction here")
        return
    try:
        os.symlink(target, link, target_is_directory=is_dir)
    except (OSError, NotImplementedError):
        pytest.skip("cannot create a symlink here (needs Developer Mode or admin on Windows)")


def external_dir(tmp_path):
    ext = tmp_path / "external"
    ext.mkdir()
    (ext / "keep.txt").write_bytes(b"keep")
    return ext


def assert_refused(capsys, proj, *flags):
    code, _, err = cli(capsys, proj, "--baseline-save", *flags)
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "refusing" in err


@pytest.mark.parametrize("kind", ["junction", "symlink"])
@pytest.mark.parametrize("flags", [(), ("--force",)])
def test_baseline_save_refuses_out_that_is_a_link(proj, tmp_path, capsys, kind, flags):
    ext = external_dir(tmp_path)
    make_link(kind, proj / "out", ext)
    assert_refused(capsys, proj, *flags)
    assert sorted(p.name for p in ext.iterdir()) == ["keep.txt"]


@pytest.mark.parametrize("kind", ["junction", "symlink"])
@pytest.mark.parametrize("flags", [(), ("--force",)])
def test_baseline_save_refuses_verify_link_to_external_dir(proj, tmp_path, capsys, kind, flags):
    ext = external_dir(tmp_path)
    (proj / "out").mkdir()
    make_link(kind, proj / "out" / "verify", ext)
    assert_refused(capsys, proj, *flags)
    assert sorted(p.name for p in ext.iterdir()) == ["keep.txt"]


@pytest.mark.parametrize("kind", ["junction", "symlink"])
@pytest.mark.parametrize("flags", [(), ("--force",)])
def test_baseline_save_refuses_verify_link_to_another_dir_inside_out(proj, capsys, kind, flags):
    sibling = proj / "out" / "server"
    sibling.mkdir(parents=True)
    make_link(kind, proj / "out" / "verify", sibling)
    assert_refused(capsys, proj, *flags)
    assert list(sibling.iterdir()) == []


@pytest.mark.parametrize("flags", [(), ("--force",)])
def test_baseline_save_refuses_baseline_file_symlink_to_external_file(proj, tmp_path, capsys, flags):
    victim = tmp_path / "victim.txt"
    victim.write_bytes(b"precious")
    (proj / "out" / "verify").mkdir(parents=True)
    make_link("symlink", baseline_file(proj), victim, is_dir=False)
    assert_refused(capsys, proj, *flags)
    assert victim.read_bytes() == b"precious"


def test_baseline_save_refuses_tmp_file_symlink_to_external_file(proj, tmp_path, capsys):
    victim = tmp_path / "victim.txt"
    victim.write_bytes(b"precious")
    (proj / "out" / "verify").mkdir(parents=True)
    make_link("symlink", proj / "out" / "verify" / "baseline.json.tmp", victim, is_dir=False)
    assert_refused(capsys, proj, "--force")
    assert victim.read_bytes() == b"precious"
    assert not baseline_file(proj).exists()


@pytest.mark.parametrize("kind", ["junction", "symlink"])
@pytest.mark.parametrize("flags", [(), ("--force",)])
def test_baseline_save_refuses_dangling_out_link(proj, tmp_path, capsys, kind, flags):
    ext = tmp_path / "gone"
    ext.mkdir()
    make_link(kind, proj / "out", ext)
    ext.rmdir()  # now dangling
    assert_refused(capsys, proj, *flags)
    assert not ext.exists()  # nothing recreated through the link


def test_baseline_save_refuses_out_that_is_a_file(proj, capsys):
    write(proj, "out", "not a dir")
    assert_refused(capsys, proj)
    assert (proj / "out").read_text() == "not a dir"


# --- baseline comparison ----------------------------------------------------------------------------

def test_pre_existing_changes_are_not_attributed_to_the_task(proj, capsys):
    write(proj, "tools/verify/a.py", "a = 2\n")   # pre-existing, modified
    write(proj, "old_untracked.txt", "old\n")      # pre-existing, untracked
    cli(capsys, proj, "--baseline-save")
    write(proj, "tools/verify/new.py", "n = 1\n")           # new
    write(proj, "tools/verify/a.py", "a = 3\n")             # pre-existing path, content changed
    r = one(proj, "baseline", baseline=True)
    assert r.status is Status.PASS
    assert "new 1;" in r.detail and "content-changed 1;" in r.detail
    assert "pre-existing unchanged 1 (not attributed)" in r.detail
    assert "new: tools/verify/new.py" in r.detail and "content-changed: tools/verify/a.py" in r.detail
    assert "old_untracked.txt" not in r.detail


def test_nothing_changed_since_baseline_attributes_nothing(proj, capsys):
    write(proj, "pre.txt")
    cli(capsys, proj, "--baseline-save")
    r = one(proj, "baseline", baseline=True)
    assert r.status is Status.PASS and "new 0;" in r.detail and "content-changed 0;" in r.detail


def test_reverted_path_is_reported_as_cleared(proj, capsys):
    write(proj, "pre.txt")
    cli(capsys, proj, "--baseline-save")
    (proj / "pre.txt").unlink()
    assert "cleared since baseline 1" in one(proj, "baseline", baseline=True).detail


def test_stale_head_is_incomplete_not_a_verdict(proj, capsys):
    confirm_paths(proj)
    write(proj, "pre.txt")
    cli(capsys, proj, "--baseline-save")
    write(proj, "tools/verify/other.py")
    git(proj, "add", "tools/verify/other.py")
    git(proj, "commit", "-q", "-m", "move head")
    r = one(proj, "baseline", baseline=True)
    assert r.status is Status.INCOMPLETE and "stale baseline" in r.detail
    scope = one(proj, "milestone_scope", baseline=True)
    assert scope.status is Status.INCOMPLETE and "stale" in scope.detail
    code, _, _ = cli(capsys, proj, "--only", "git.baseline,git.milestone_scope", "--baseline")
    assert code == core.EXIT_USAGE_OR_INCOMPLETE


def test_missing_or_malformed_baseline_is_incomplete(proj):
    assert one(proj, "baseline", baseline=True).status is Status.INCOMPLETE  # nothing saved
    for content in ("not json", "{}", json.dumps({"format": cg.BASELINE_FORMAT, "head": "abc", "entries": {}}),
                    json.dumps({"format": cg.BASELINE_FORMAT, "head": "a" * 40, "entries": {"x": {"xy": 1}}})):
        write(proj, "out/verify/baseline.json", content)
        r = one(proj, "baseline", baseline=True)
        assert r.status is Status.INCOMPLETE and ("malformed" in r.detail or "unreadable" in r.detail)


def test_without_baseline_flag_there_is_no_scope_verdict(proj, capsys):
    confirm_paths(proj)
    cli(capsys, proj, "--baseline-save")
    write(proj, "outside/everything.txt")
    assert one(proj, "baseline").status is Status.SKIP
    scope = one(proj, "milestone_scope")
    assert scope.status is Status.SKIP and "no scope verdict" in scope.detail


# --- milestones ------------------------------------------------------------------------------------------

def confirm_paths(root, milestone="M2.0"):
    """Simulate the owner confirming the allowed paths (fixture only; the shipped file stays PROPOSED)."""
    path = root / "tools" / "verify" / "milestones.json"
    config = json.loads(path.read_text(encoding="utf-8"))
    config["milestones"][milestone]["allowed_paths_status"] = "confirmed"
    path.write_text(json.dumps(config), encoding="utf-8")


def shipped():
    return json.loads((cg.Path(__file__).resolve().parents[1] / "milestones.json").read_text(encoding="utf-8"))


def test_shipped_config_defaults_to_m2_0_and_only_m2_0_is_approved():
    config = shipped()
    assert cg._milestone_problem(config) == ""
    assert config["default"] == "M2.0" == verify.DEFAULT_MILESTONE
    assert [m for m, c in config["milestones"].items() if c["approved"]] == ["M2.0"]
    assert all(c["allowed_paths_status"] == "proposed" for c in config["milestones"].values())  # none confirmed
    assert config["milestones"]["M2.0"]["allowed_paths"] == [
        "CLOUD.md", "server/README.md", ".claude/memory/**", ".claude/reports/**", "tools/verify/**"]
    for cfg in config["milestones"].values():
        assert isinstance(cfg["allowed_paths"], list) and isinstance(cfg["forbidden_imports"], list)


def test_approved_milestone_in_scope_passes_and_out_of_scope_fails(proj, capsys):
    confirm_paths(proj)
    cli(capsys, proj, "--baseline-save")
    write(proj, "tools/verify/new.py")
    write(proj, "CLOUD.md", "doc\n")
    r = one(proj, "milestone_scope", baseline=True)
    assert r.status is Status.PASS and "path scope only" in r.detail
    write(proj, "scripts/src/app.cpp", "int x;\n")
    r = one(proj, "milestone_scope", baseline=True)
    assert r.status is Status.FAIL and "scripts/src/app.cpp" in r.detail and "tools/verify/new.py" not in r.detail


def test_pre_existing_out_of_scope_changes_do_not_fail_the_scope_check(proj, capsys):
    confirm_paths(proj)
    write(proj, "scripts/src/app.cpp", "int x;\n")  # already there when the baseline is taken
    cli(capsys, proj, "--baseline-save")
    write(proj, "tools/verify/new.py")
    assert one(proj, "milestone_scope", baseline=True).status is Status.PASS


@pytest.mark.parametrize("milestone", ["M2.1", "M3"])
def test_unapproved_milestone_is_incomplete(proj, capsys, milestone):
    cli(capsys, proj, "--baseline-save")
    r = one(proj, "milestone_scope", milestone=milestone, baseline=True)
    assert r.status is Status.INCOMPLETE and "not approved" in r.detail
    assert one(proj, "milestone_scope", milestone=milestone).status is Status.INCOMPLETE  # even without a baseline
    code, out, _ = cli(capsys, proj, "--only", "git.milestone_scope", "--milestone", milestone)
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "[INCOMPLETE] git.milestone_scope" in out


def test_unknown_milestone_is_incomplete(proj):
    r = one(proj, "milestone_scope", milestone="M9")
    assert r.status is Status.INCOMPLETE and "not configured" in r.detail


def test_branch_name_never_establishes_approval(proj, capsys):
    git(proj, "checkout", "-q", "-b", "m3-approved-by-owner")
    r = one(proj, "milestone_scope", milestone="M3")
    assert r.status is Status.INCOMPLETE
    assert "(informational only)" in one(proj, "status_facts").detail


def test_missing_milestone_config_is_never_a_pass(proj):
    (proj / "tools/verify/milestones.json").unlink()
    r = one(proj, "milestone_scope")
    assert r.status is Status.INCOMPLETE and "missing" in r.detail


@pytest.mark.parametrize("mutate,needle", [
    (lambda c: c.update(format="other"), "format"),
    (lambda c: c.update(default="M7"), "default"),
    (lambda c: c.update(milestones={}), "milestones"),
    (lambda c: c["milestones"]["M2.0"].update(approved="yes"), "approved"),
    (lambda c: c["milestones"]["M2.0"].update(allowed_paths="tools/**"), "allowed_paths"),
    (lambda c: c["milestones"]["M2.0"].update(allowed_paths=[]), "no allowed_paths"),
    (lambda c: c["milestones"]["M2.0"].update(allowed_paths_status="approved"), "allowed_paths_status"),
    (lambda c: c["milestones"]["M2.0"].pop("allowed_paths_status"), "allowed_paths_status"),
    (lambda c: c["milestones"]["M2.0"].pop("forbidden_imports"), "forbidden_imports"),
    (lambda c: c["milestones"].update({"bad id": c["milestones"]["M3"]}), "invalid milestone"),
])
def test_malformed_milestone_config_fails(proj, mutate, needle):
    config = shipped()
    mutate(config)
    write(proj, "tools/verify/milestones.json", json.dumps(config))
    r = one(proj, "milestone_scope")
    assert r.status is Status.FAIL and needle in r.detail


def test_unparseable_and_duplicate_key_config_fails(proj):
    write(proj, "tools/verify/milestones.json", "{not json")
    assert one(proj, "milestone_scope").status is Status.FAIL
    write(proj, "tools/verify/milestones.json", '{"format": "a", "format": "b"}')
    r = one(proj, "milestone_scope")
    assert r.status is Status.FAIL and "not valid JSON" in r.detail


# --- write isolation ---------------------------------------------------------------------------------------

def test_git_checks_and_cli_change_nothing_outside_out_verify(proj, capsys):
    write(proj, "scratch.txt", "s\n")
    git(proj, "add", "scratch.txt")
    write(proj, "conf.txt", f"k = {FAKE_TOKEN}\n")
    git(proj, "add", "conf.txt")
    before = fs.snapshot(proj)
    for args in ([], ["--json"], ["--only", "git"], ["--baseline"], ["--full"], ["--list"]):
        cli(capsys, proj, *args)
    assert not fs.has_changes(fs.diff(before, fs.snapshot(proj)))
    assert fs.snapshot(proj) == before  # stricter: not even out/verify was touched


def test_baseline_save_writes_only_the_baseline_file(proj, capsys):
    write(proj, "scratch.txt")
    git_before = {p: v for p, v in fs.snapshot(proj).items() if p.startswith(".git/")}
    before = fs.snapshot(proj)
    cli(capsys, proj, "--baseline-save")
    after = fs.snapshot(proj)
    assert sorted(set(after) - set(before)) == ["out", "out/verify", "out/verify/baseline.json"]
    assert [p for p in before if before[p] != after.get(p)] == []
    assert {p: v for p, v in after.items() if p.startswith(".git/")} == git_before
    assert not (proj / "out" / "verify" / "baseline.json.tmp").exists()


def test_gitro_allowlist_still_rejects_mutations_used_by_nothing_here():
    for bad in (["add", "."], ["commit", "-m", "x"], ["reset", "--hard"], ["config", "user.name", "x"]):
        with pytest.raises(gitro.GitRejected):
            gitro.classify(bad)


# --- proposed (unconfirmed) allowed paths ----------------------------------------------------------------

def test_proposed_paths_keep_scope_incomplete_with_and_without_baseline(proj, capsys):
    r = one(proj, "milestone_scope")  # no baseline
    assert r.status is Status.INCOMPLETE and "PROPOSED" in r.detail and "not owner-confirmed" in r.detail
    for path in ("CLOUD.md", "server/README.md", ".claude/memory/**", ".claude/reports/**", "tools/verify/**"):
        assert path in r.detail
    cli(capsys, proj, "--baseline-save")
    write(proj, "tools/verify/new.py")           # inside the proposed scope
    write(proj, "scripts/src/app.cpp", "x\n")    # and a path outside it
    r = one(proj, "milestone_scope", baseline=True)
    assert r.status is Status.INCOMPLETE and "PROPOSED" in r.detail  # neither PASS nor FAIL
    code, out, _ = cli(capsys, proj, "--only", "git.milestone_scope", "--baseline")
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "[INCOMPLETE] git.milestone_scope" in out


def test_confirming_the_paths_enables_a_verdict(proj, capsys):
    confirm_paths(proj)
    cli(capsys, proj, "--baseline-save")
    write(proj, "tools/verify/new.py")
    assert one(proj, "milestone_scope", baseline=True).status is Status.PASS


def test_other_milestone_approval_states_are_unchanged(proj):
    states = {m: c["approved"] for m, c in shipped()["milestones"].items()}
    assert states == {"M2.0": True, "M2.1": False, "M3": False}
    for m in ("M2.1", "M3"):
        assert "not approved" in one(proj, "milestone_scope", milestone=m).detail
