import json

import pytest

import core
from core import Result, Status


def r(group, id_, status, detail=""):
    return Result(group, id_, status, detail)


def make_registry():
    reg = core.Registry()
    reg.register("b", "two", lambda ctx: r("b", "two", Status.PASS))
    reg.register("a", "one", lambda ctx: r("a", "one", Status.PASS))
    reg.register("a", "slow", lambda ctx: r("a", "slow", Status.PASS), slow=True)
    return reg


def test_status_values_are_the_approved_set():
    assert [s.value for s in core.STATUS_ORDER] == ["PASS", "FAIL", "SKIP", "INCOMPLETE", "NOT RUN"]


def test_result_serialization_is_stable():
    result = Result("g", "i", Status.NOT_RUN, "why", core.KIND_HEURISTIC)
    assert result.to_dict() == {"detail": "why", "group": "g", "id": "i",
                                "kind": "heuristic", "status": "NOT RUN"}
    assert result.full_id == "g.i"


def test_duplicate_check_id_is_rejected():
    reg = core.Registry()
    reg.register("g", "i", lambda ctx: None)
    with pytest.raises(ValueError):
        reg.register("g", "i", lambda ctx: None)


def test_registry_order_is_group_then_id_regardless_of_registration_order():
    assert [s.full_id for s in make_registry().specs()] == ["a.one", "a.slow", "b.two"]


def test_decorator_registers():
    reg = core.Registry()

    @reg.check("g", "x")
    def fn(ctx):
        return r("g", "x", Status.PASS)

    assert [s.full_id for s in reg.specs()] == ["g.x"]
    assert fn(None).status is Status.PASS


def test_parse_only():
    assert core.parse_only(None) == []
    assert core.parse_only("a, b.c") == ["a", "b.c"]
    for bad in ("", "  ", "a,", ",a", "a,,b"):
        with pytest.raises(core.SelectionError):
            core.parse_only(bad)


def test_unknown_selector_is_rejected():
    with pytest.raises(core.SelectionError):
        core.build_plan(make_registry(), ["nope"], full=False)
    with pytest.raises(core.SelectionError):
        core.build_plan(make_registry(), ["a.nope"], full=False)


def test_selectors_filter_by_group_and_by_id():
    plan = core.build_plan(make_registry(), ["a.one"], full=False)
    assert [s.full_id for s in plan.runnable] == ["a.one"]
    assert plan.planned == 1
    plan = core.build_plan(make_registry(), ["a"], full=True)
    assert [s.full_id for s in plan.runnable] == ["a.one", "a.slow"]


def test_slow_checks_skip_without_full_and_run_with_full():
    ctx = object()
    plan = core.build_plan(make_registry(), [], full=False)
    results = core.run_plan(plan, ctx)
    by_id = {x.full_id: x for x in results}
    assert by_id["a.slow"].status is Status.SKIP
    assert "--full" in by_id["a.slow"].detail
    assert plan.planned == 3
    plan = core.build_plan(make_registry(), [], full=True)
    assert {x.full_id: x.status for x in core.run_plan(plan, ctx)}["a.slow"] is Status.PASS


def test_pending_groups_are_not_run_and_selectable():
    pending = (("m1", "later"), ("git", "later"))
    plan = core.build_plan(core.Registry(), [], False, pending)
    results = core.run_plan(plan, object())
    assert [(x.full_id, x.status) for x in results] == [("git.pending", Status.NOT_RUN),
                                                         ("m1.pending", Status.NOT_RUN)]
    only = core.build_plan(core.Registry(), ["m1"], False, pending)
    assert [x.full_id for x in only.pending] == ["m1.pending"]


def test_raising_check_becomes_fail_and_others_still_run():
    reg = core.Registry()

    def boom(ctx):
        raise RuntimeError("bad")

    reg.register("a", "boom", boom)
    reg.register("a", "ok", lambda ctx: r("a", "ok", Status.PASS))
    results = core.run_plan(core.build_plan(reg, [], False), object())
    by_id = {x.id: x for x in results}
    assert by_id["boom"].status is Status.FAIL and "RuntimeError: bad" in by_id["boom"].detail
    assert by_id["ok"].status is Status.PASS


def test_non_result_return_values_fail():
    reg = core.Registry()
    reg.register("a", "none", lambda ctx: None)
    reg.register("a", "empty", lambda ctx: [])
    reg.register("a", "many", lambda ctx: [r("a", "many", Status.PASS), r("a", "many2", Status.SKIP)])
    got = {x.id: x.status for x in core.run_plan(core.build_plan(reg, [], False), object())}
    assert got == {"empty": Status.FAIL, "many": Status.PASS, "many2": Status.SKIP, "none": Status.FAIL}


def test_tally_and_completed_count_never_count_skip_as_pass():
    results = [r("a", "1", Status.PASS), r("a", "2", Status.SKIP), r("a", "3", Status.FAIL),
               r("a", "4", Status.INCOMPLETE), r("a", "5", Status.NOT_RUN)]
    assert core.tally(results) == {"PASS": 1, "FAIL": 1, "SKIP": 1, "INCOMPLETE": 1, "NOT RUN": 1}
    assert core.completed_count(results) == 3  # PASS + FAIL + SKIP only


def test_progress_bar():
    assert core.progress_bar(0, 10) == "[----------]"
    assert core.progress_bar(5, 10) == "[#####-----]"
    assert core.progress_bar(10, 10) == "[##########]"
    assert core.progress_bar(0, 0) == "[----------]"
    assert core.progress_bar(99, 10) == "[##########]"
    assert core.progress_bar(1, 3, width=6) == "[##----]"


@pytest.mark.parametrize("statuses,expected", [
    ([], core.EXIT_OK),
    ([Status.PASS, Status.SKIP], core.EXIT_OK),
    ([Status.PASS, Status.FAIL], core.EXIT_FAIL),
    ([Status.FAIL, Status.INCOMPLETE, Status.NOT_RUN], core.EXIT_FAIL),
    ([Status.PASS, Status.INCOMPLETE], core.EXIT_USAGE_OR_INCOMPLETE),
    ([Status.SKIP, Status.NOT_RUN], core.EXIT_USAGE_OR_INCOMPLETE),
])
def test_exit_codes(statuses, expected):
    results = [r("g", str(i), s) for i, s in enumerate(statuses)]
    assert core.exit_code(results) == expected


def test_human_output_lists_every_status_and_completed_ratio():
    results = [r("a", "1", Status.PASS), r("a", "2", Status.SKIP, "why"), r("b", "3", Status.NOT_RUN)]
    text = core.format_human(results, planned=3)
    assert "[PASS] a.1" in text and "[SKIP] a.2 - why" in text and "[NOT RUN] b.3" in text
    assert "2/3 completed | PASS 1, FAIL 0, SKIP 1, INCOMPLETE 0, NOT RUN 1" in text


def test_heuristic_results_are_labelled():
    text = core.format_human([Result("a", "1", Status.PASS, "", core.KIND_HEURISTIC)], 1)
    assert "[PASS] a.1 (heuristic)" in text


def test_json_is_valid_sorted_and_byte_stable():
    results = [r("b", "2", Status.FAIL), r("a", "1", Status.PASS)]
    first = core.format_json(results, 2, {"milestone": "M2.0"})
    assert first == core.format_json(list(results), 2, {"milestone": "M2.0"})
    data = json.loads(first)
    assert data["summary"] == {"completed": 2, "counts": {"PASS": 1, "FAIL": 1, "SKIP": 0,
                                                          "INCOMPLETE": 0, "NOT RUN": 0},
                               "planned": 2, "total": 2}
    assert data["milestone"] == "M2.0"
    assert list(data) == sorted(data)


# --- run_allowed: the only subprocess boundary -----------------------------------------------

import subprocess
import sys
from pathlib import Path


def test_allowlist_has_exactly_the_two_fixed_entries():
    assert set(core.ALLOWED_COMMANDS) == {"server.check", "memory.verify"}
    assert core.ALLOWED_COMMANDS["server.check"].script_parts == ("server", "check.py")
    assert core.ALLOWED_COMMANDS["server.check"].timeout == 900
    assert core.ALLOWED_COMMANDS["memory.verify"].script_parts == (".claude", "scripts", "verify_memory.py")
    assert core.ALLOWED_COMMANDS["memory.verify"].timeout == 120


@pytest.mark.parametrize("bad", ["", "git.status", "server", "memory.verify ", "../x", "python -c 1", sys.executable])
def test_unknown_and_arbitrary_ids_rejected_before_any_process(monkeypatch, tmp_path, bad):
    def boom(*a, **k):
        raise AssertionError("a process must not start")
    monkeypatch.setattr(core.subprocess, "run", boom)
    with pytest.raises(ValueError):
        core.run_allowed(bad, tmp_path, {})


def test_signature_accepts_no_executable_arguments_or_cwd(tmp_path):
    for kw in ({"cmd": ["x"]}, {"args": ["x"]}, {"cwd": "/"}, {"timeout": 1}, {"shell": True}):
        with pytest.raises(TypeError):
            core.run_allowed("server.check", tmp_path, {}, **kw)


@pytest.mark.parametrize("spec_id", ["server.check", "memory.verify"])
def test_exact_command_and_process_options(monkeypatch, tmp_path, spec_id):
    seen = {}

    class P:
        returncode = 3
        stdout = b"out"

    def fake(cmd, **kw):
        seen["cmd"], seen["kw"] = cmd, kw
        return P()
    monkeypatch.setattr(core.subprocess, "run", fake)
    env = {"A": "1"}
    outcome = core.run_allowed(spec_id, tmp_path, env)
    spec = core.ALLOWED_COMMANDS[spec_id]
    assert seen["cmd"] == [sys.executable, "-B", str(tmp_path.joinpath(*spec.script_parts))]
    kw = seen["kw"]
    assert kw["shell"] is False and kw["cwd"] == str(tmp_path) and kw["env"] == env
    assert kw["stdin"] == subprocess.DEVNULL and kw["stderr"] == subprocess.STDOUT
    assert kw["stdout"] == subprocess.PIPE and kw["timeout"] == spec.timeout
    assert outcome == core.RunOutcome(3, "out")


def test_timeout_keeps_partial_output(monkeypatch, tmp_path):
    def slow(cmd, **k):
        raise subprocess.TimeoutExpired(cmd, 1, output=b"partial \xff")
    monkeypatch.setattr(core.subprocess, "run", slow)
    o = core.run_allowed("server.check", tmp_path, {})
    assert o.timed_out and o.returncode is None and o.output.startswith("partial") and not o.launch_error


def test_timeout_without_output(monkeypatch, tmp_path):
    def slow(cmd, **k):
        raise subprocess.TimeoutExpired(cmd, 1)
    monkeypatch.setattr(core.subprocess, "run", slow)
    o = core.run_allowed("memory.verify", tmp_path, {})
    assert o.timed_out and o.output == ""


def test_launch_failure_reports_type_name_only(monkeypatch, tmp_path):
    def boom(*a, **k):
        raise FileNotFoundError("secret path detail")
    monkeypatch.setattr(core.subprocess, "run", boom)
    o = core.run_allowed("server.check", tmp_path, {})
    assert o.launch_error == "FileNotFoundError" and o.returncode is None and not o.timed_out
    assert "secret" not in repr(o)


def test_real_process_runs_fixed_script_with_merged_output(tmp_path):
    (tmp_path / "server").mkdir()
    (tmp_path / "server" / "check.py").write_text(
        "import sys\nprint('o'); print('e', file=sys.stderr); sys.exit(1)\n", encoding="utf-8")
    import os
    o = core.run_allowed("server.check", tmp_path, os.environ)
    assert o.returncode == 1 and "o" in o.output and "e" in o.output
