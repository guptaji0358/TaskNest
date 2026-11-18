"""TaskNest verification toolkit CLI (Step 3.1 foundation).

    python tools/verify/verify.py [--list] [--only GROUPS|group.id,...] [--full]
                                  [--json] [--milestone M2.0]
                                  [--baseline | --baseline-save [--force]]

Read-only, except an explicit --baseline-save, which writes only out/verify/baseline.json.
Implemented groups: contract, m1, git, memory, server (server runs only with --full). Groups that are not implemented yet are reported NOT RUN, never PASS.
Exit codes: 0 no FAIL/INCOMPLETE/NOT RUN; 1 any FAIL; 2 usage error, missing repo
root, or any INCOMPLETE / NOT RUN without a FAIL.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # keep __pycache__ out of tools/verify/

import checks_git  # noqa: E402
import checks_memory  # noqa: E402
import checks_server  # noqa: E402
import checks_contract  # noqa: E402  (sibling modules; the script directory is on sys.path)
import checks_m1  # noqa: E402
import core  # noqa: E402
import repo  # noqa: E402

DEFAULT_MILESTONE = "M2.0"
MILESTONE_RE = re.compile(r"^M\d+(\.\d+)*$")

# Groups in the approved design that are not implemented in this phase.
PENDING_GROUPS: tuple[tuple[str, str], ...] = ()  # all approved groups are implemented as of Step 3.5

MILESTONE_NOTE = ("milestone selects the scope rules in tools/verify/milestones.json; approval comes only "
                  "from that file, never from the branch name")

DEFAULT_REGISTRY = core.Registry()
checks_contract.register(DEFAULT_REGISTRY)
checks_m1.register(DEFAULT_REGISTRY)
checks_git.register(DEFAULT_REGISTRY)
checks_memory.register(DEFAULT_REGISTRY)
checks_server.register(DEFAULT_REGISTRY)


class _Parser(argparse.ArgumentParser):
    def error(self, message: str):  # usage errors exit 2 with a message on stderr
        self.print_usage(sys.stderr)
        self.exit(core.EXIT_USAGE_OR_INCOMPLETE, f"verify.py: error: {message}\n")


def build_parser() -> argparse.ArgumentParser:
    parser = _Parser(prog="verify.py", description="TaskNest verification toolkit (read-only).")
    parser.add_argument("--list", action="store_true", help="list planned checks; run nothing")
    parser.add_argument("--only", metavar="SEL", help="comma-separated groups or group.id values")
    parser.add_argument("--full", action="store_true", help="also run slow checks")
    parser.add_argument("--json", action="store_true", help="machine-readable output on stdout")
    parser.add_argument("--milestone", default=DEFAULT_MILESTONE, metavar="ID",
                        help=f"milestone label (default {DEFAULT_MILESTONE})")
    parser.add_argument("--baseline", action="store_true",
                        help="attribute changes against the saved out/verify/baseline.json")
    parser.add_argument("--baseline-save", action="store_true",
                        help="save the current git state to out/verify/baseline.json, then exit")
    parser.add_argument("--force", action="store_true", help="with --baseline-save: overwrite an existing baseline")
    return parser


def _usage_error(message: str) -> int:
    sys.stderr.write(f"verify.py: error: {message}\n")
    return core.EXIT_USAGE_OR_INCOMPLETE


def main(argv: list[str] | None = None, *, start: Path | str | None = None,
         registry: core.Registry | None = None,
         pending: tuple[tuple[str, str], ...] | None = None) -> int:
    registry = DEFAULT_REGISTRY if registry is None else registry
    pending = PENDING_GROUPS if pending is None else pending
    try:
        args = build_parser().parse_args(argv)
    except SystemExit as exc:
        return int(exc.code) if isinstance(exc.code, int) else core.EXIT_USAGE_OR_INCOMPLETE

    if not MILESTONE_RE.match(args.milestone):
        return _usage_error(f"invalid --milestone value: {args.milestone!r} (expected e.g. M2.0)")
    if args.force and not args.baseline_save:
        return _usage_error("--force is only valid with --baseline-save")
    if args.baseline_save and (args.baseline or args.only or args.list or args.json or args.full):
        return _usage_error("--baseline-save cannot be combined with --baseline, --only, --list, --json or --full")
    if args.baseline_save:
        return _baseline_save(args.force, start)
    try:
        selectors = core.parse_only(args.only)
        plan = core.build_plan(registry, selectors, args.full, pending)
    except core.SelectionError as exc:
        return _usage_error(str(exc))

    if args.list:
        return _print_list(plan, args.json)

    try:
        root = repo.find_root(start if start is not None else Path(__file__).parent)
    except repo.RepoNotFound as exc:
        return _usage_error(str(exc))
    ctx = repo.Ctx(root=root, milestone=args.milestone, baseline=args.baseline)

    results = core.run_plan(plan, ctx)
    if args.json:
        print(core.format_json(results, plan.planned,
                               {"milestone": args.milestone, "milestone_note": MILESTONE_NOTE}))
    else:
        print(core.format_human(results, plan.planned,
                                (f"milestone: {args.milestone} ({MILESTONE_NOTE})",)))
    return core.exit_code(results)


def _baseline_save(force: bool, start) -> int:
    try:
        root = repo.find_root(start if start is not None else Path(__file__).parent)
    except repo.RepoNotFound as exc:
        return _usage_error(str(exc))
    ok, message = checks_git.save_baseline(root, force)
    if not ok:
        return _usage_error(message)
    print(message)
    return core.EXIT_OK


def _print_list(plan: core.Plan, as_json: bool) -> int:
    import json
    rows = [{"id": s.full_id, "slow": s.slow, "state": "implemented"}
            for s in plan.runnable + plan.skipped_slow]
    rows += [{"id": r.full_id, "slow": False, "state": "pending"} for r in plan.pending]
    rows.sort(key=lambda row: row["id"])
    if as_json:
        print(json.dumps({"checks": rows}, sort_keys=True, indent=2))
    else:
        for row in rows:
            print(f"{row['id']} [{row['state']}{', slow' if row['slow'] else ''}]")
        print(f"{len(rows)} planned")
    return core.EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
