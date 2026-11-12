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
