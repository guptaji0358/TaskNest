import json

import pytest

import core
import verify
from core import Result, Status


def run_main(capsys, argv, **kwargs):
    code = verify.main(argv, **kwargs)
    captured = capsys.readouterr()
    return code, captured.out, captured.err


def passing_registry():
    reg = core.Registry()
    reg.register("g", "ok", lambda ctx: Result("g", "ok", Status.PASS))
    return reg


def test_default_run_reports_every_group_not_run_and_exits_2(capsys, fake_repo):
    code, out, _ = run_main(capsys, [], start=fake_repo, registry=core.Registry())
    assert code == core.EXIT_USAGE_OR_INCOMPLETE
    n = len(verify.PENDING_GROUPS)
    for group, _reason in verify.PENDING_GROUPS:
        assert f"[NOT RUN] {group}.pending" in out
    assert "PASS" not in out.split("completed |")[0].replace("PASS 0", "")  # no fabricated PASS lines
    assert f"0/{n} completed | PASS 0, FAIL 0, SKIP 0, INCOMPLETE 0, NOT RUN {n}" in out


def test_all_pass_exits_0(capsys, fake_repo):
    code, out, _ = run_main(capsys, [], start=fake_repo, registry=passing_registry(), pending=())
    assert code == core.EXIT_OK and "1/1 completed" in out


def test_failing_check_exits_1(capsys, fake_repo):
    reg = core.Registry()
    reg.register("g", "bad", lambda ctx: Result("g", "bad", Status.FAIL, "x"))
    code, _, _ = run_main(capsys, [], start=fake_repo, registry=reg, pending=())
    assert code == core.EXIT_FAIL


def test_json_output_is_valid_deterministic_and_includes_milestone(capsys, fake_repo):
    code, first, _ = run_main(capsys, ["--json"], start=fake_repo, registry=core.Registry())
    _, second, _ = run_main(capsys, ["--json"], start=fake_repo, registry=core.Registry())
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and first == second
    data = json.loads(first)
    assert data["milestone"] == "M2.0"
    n = len(verify.PENDING_GROUPS)
    assert data["summary"]["counts"]["NOT RUN"] == n and data["summary"]["counts"]["PASS"] == 0
    assert [x["id"] for x in data["results"]] == ["pending"] * n
    assert [x["group"] for x in data["results"]] == sorted(g for g, _ in verify.PENDING_GROUPS)


def test_list_runs_nothing_and_exits_0(capsys):
    code, out, _ = run_main(capsys, ["--list"])
    assert code == core.EXIT_OK
    assert "memory.pending [pending]" in out and out.strip().endswith(f"{len(verify.DEFAULT_REGISTRY.specs()) + len(verify.PENDING_GROUPS)} planned")
    assert "contract.refs_resolve [implemented]" in out and "m1.live_tables [implemented]" in out


def test_list_json(capsys):
    code, out, _ = run_main(capsys, ["--list", "--json"])
    data = json.loads(out)
    assert code == 0 and [row["id"] for row in data["checks"]] == sorted(row["id"] for row in data["checks"])


def test_only_filters_and_unknown_selector_is_usage_error(capsys, fake_repo):
    code, out, _ = run_main(capsys, ["--only", "memory"], start=fake_repo)
    assert "memory.pending" in out and "m1." not in out and "0/1 completed" in out
    code, out, _ = run_main(capsys, ["--only", "m1"], start=fake_repo)
    assert "m1.live_tables" in out and "memory.pending" not in out and "contract." not in out
    code, _, err = run_main(capsys, ["--only", "bogus"], start=fake_repo)
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "unknown check or group: bogus" in err
    code, _, err = run_main(capsys, ["--only", "m1,"], start=fake_repo)
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "--only" in err


def test_full_flag_enables_slow_checks(capsys, fake_repo):
    reg = core.Registry()
    reg.register("g", "slow", lambda ctx: Result("g", "slow", Status.PASS), slow=True)
    code, out, _ = run_main(capsys, [], start=fake_repo, registry=reg, pending=())
    assert "[SKIP] g.slow" in out and code == core.EXIT_OK  # SKIP is completed, not PASS
    assert "PASS 0, FAIL 0, SKIP 1" in out
    code, out, _ = run_main(capsys, ["--full"], start=fake_repo, registry=reg, pending=())
    assert "[PASS] g.slow" in out


@pytest.mark.parametrize("value", ["", "m2.0", "M", "M2.", "2.0", "M2.0 ", "M2.0; rm"])
def test_invalid_milestone_is_usage_error(capsys, fake_repo, value):
    code, _, err = run_main(capsys, ["--milestone", value], start=fake_repo)
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "invalid --milestone" in err


def test_valid_milestone_is_recorded_and_not_validated(capsys, fake_repo):
    _, out, _ = run_main(capsys, ["--milestone", "M9.1.2", "--json"], start=fake_repo)
    data = json.loads(out)
    assert data["milestone"] == "M9.1.2" and "never from the branch name" in data["milestone_note"]


def test_unknown_argument_is_usage_error(capsys):
    code, _, err = run_main(capsys, ["--nope"])
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "unrecognized arguments" in err


def test_missing_root_is_usage_error(capsys, tmp_path):
    code, _, err = run_main(capsys, [], start=tmp_path)
    assert code == core.EXIT_USAGE_OR_INCOMPLETE and "no repository root" in err
