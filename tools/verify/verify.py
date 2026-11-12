"""TaskNest verification toolkit CLI (Step 3.1 foundation).

    python tools/verify/verify.py [--list] [--only GROUPS|group.id,...] [--full]
                                  [--json] [--milestone M2.0]

Read-only: this phase writes nothing to disk and runs no real checks yet. Groups
that are not implemented are reported NOT RUN, never PASS.
Exit codes: 0 no FAIL/INCOMPLETE/NOT RUN; 1 any FAIL; 2 usage error, missing repo
root, or any INCOMPLETE / NOT RUN without a FAIL.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # keep __pycache__ out of tools/verify/

import core  # noqa: E402  (sibling module; the script directory is on sys.path)
import repo  # noqa: E402

DEFAULT_MILESTONE = "M2.0"
MILESTONE_RE = re.compile(r"^M\d+(\.\d+)*$")

# Groups in the approved design that are not implemented in this phase.
PENDING_GROUPS = (
    ("contract", "not implemented in Step 3.1"),
    ("git", "not implemented in Step 3.1"),
    ("m1", "not implemented in Step 3.1"),
    ("memory", "not implemented in Step 3.1"),
    ("server", "not implemented in Step 3.1"),
)

MILESTONE_NOTE = ("milestone recorded only; milestone configuration and approval checks "
                  "are not implemented in Step 3.1")

DEFAULT_REGISTRY = core.Registry()  # no checks registered in this phase


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
    ctx = repo.Ctx(root=root, milestone=args.milestone)

    results = core.run_plan(plan, ctx)
    if args.json:
        print(core.format_json(results, plan.planned,
                               {"milestone": args.milestone, "milestone_note": MILESTONE_NOTE}))
    else:
        print(core.format_human(results, plan.planned,
                                (f"milestone: {args.milestone} ({MILESTONE_NOTE})",)))
    return core.exit_code(results)


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
