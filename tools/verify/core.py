"""Result model, check registry, runner and reporting for the verification toolkit.

Step 3.1 foundation only: no real checks live here. Everything is deterministic
(fixed ordering, no timestamps) and the module never writes to disk.
"""
from __future__ import annotations

import enum
import json
from dataclasses import dataclass, field
from typing import Callable, Iterable, Sequence

# Exit codes (approved design).
EXIT_OK = 0
EXIT_FAIL = 1
EXIT_USAGE_OR_INCOMPLETE = 2

BAR_WIDTH = 10


class Status(str, enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    INCOMPLETE = "INCOMPLETE"
    NOT_RUN = "NOT RUN"


# PASS, FAIL and SKIP count as completed; INCOMPLETE and NOT RUN do not.
COMPLETED = (Status.PASS, Status.FAIL, Status.SKIP)
STATUS_ORDER = (Status.PASS, Status.FAIL, Status.SKIP, Status.INCOMPLETE, Status.NOT_RUN)

KIND_DETERMINISTIC = "deterministic"
KIND_HEURISTIC = "heuristic"


@dataclass(frozen=True)
class Result:
    group: str
    id: str
    status: Status
    detail: str = ""
    kind: str = KIND_DETERMINISTIC

    @property
    def full_id(self) -> str:
        return f"{self.group}.{self.id}"

    def to_dict(self) -> dict:
        return {
            "detail": self.detail,
            "group": self.group,
            "id": self.id,
            "kind": self.kind,
            "status": self.status.value,
        }


@dataclass(frozen=True)
class CheckSpec:
    group: str
    id: str
    func: Callable
    slow: bool = False
    kind: str = KIND_DETERMINISTIC

    @property
    def full_id(self) -> str:
        return f"{self.group}.{self.id}"


class Registry:
    """Holds check specs. Ids are unique; iteration order is (group, id)."""

    def __init__(self) -> None:
        self._specs: dict[str, CheckSpec] = {}

    def register(self, group: str, id: str, func: Callable, *, slow: bool = False,
                 kind: str = KIND_DETERMINISTIC) -> CheckSpec:
        spec = CheckSpec(group, id, func, slow, kind)
        if spec.full_id in self._specs:
            raise ValueError(f"duplicate check id: {spec.full_id}")
        self._specs[spec.full_id] = spec
        return spec

    def check(self, group: str, id: str, *, slow: bool = False, kind: str = KIND_DETERMINISTIC):
        def decorator(func: Callable) -> Callable:
            self.register(group, id, func, slow=slow, kind=kind)
            return func
        return decorator

    def specs(self) -> list[CheckSpec]:
        return sorted(self._specs.values(), key=lambda s: (s.group, s.id))

    def groups(self) -> list[str]:
        return sorted({s.group for s in self._specs.values()})


class SelectionError(ValueError):
    """An --only selector matched nothing known."""


def parse_only(value: str | None) -> list[str]:
    """Split a comma list; reject empty items. None or '' means no filter."""
    if value is None:
        return []
    items = [item.strip() for item in value.split(",")]
    if not value.strip() or any(not item for item in items):
        raise SelectionError("--only needs a comma-separated list of group or group.id values")
    return items


def selector_matches(selector: str, group: str, id: str) -> bool:
    return selector == group or selector == f"{group}.{id}"


def validate_selectors(selectors: Sequence[str], known: Iterable[tuple[str, str]]) -> None:
    known = list(known)
    for selector in selectors:
        if not any(selector_matches(selector, g, i) for g, i in known):
            raise SelectionError(f"unknown check or group: {selector}")


@dataclass
class Plan:
    """What a run intends to do, fixed before anything executes."""
    runnable: list[CheckSpec] = field(default_factory=list)
    skipped_slow: list[CheckSpec] = field(default_factory=list)
    pending: list[Result] = field(default_factory=list)

    @property
    def planned(self) -> int:
        return len(self.runnable) + len(self.skipped_slow) + len(self.pending)


def build_plan(registry: Registry, selectors: Sequence[str], full: bool,
               pending: Sequence[tuple[str, str]] = ()) -> Plan:
    """`pending` lists (group, reason) for groups not implemented in this phase.

    A pending group is reported NOT RUN, never PASS. A selector may name a pending
    group; selecting only implemented checks omits the pending ones.
    """
    pending_groups = [g for g, _ in pending]
    known = [(s.group, s.id) for s in registry.specs()] + [(g, "pending") for g in pending_groups]
    validate_selectors(selectors, known)

    def selected(group: str, id: str) -> bool:
        return not selectors or any(selector_matches(sel, group, id) for sel in selectors)

    plan = Plan()
    for spec in registry.specs():
        if not selected(spec.group, spec.id):
            continue
        if spec.slow and not full:
            plan.skipped_slow.append(spec)
        else:
            plan.runnable.append(spec)
    for group, reason in sorted(pending):
        if not selected(group, "pending"):
            continue
        plan.pending.append(Result(group, "pending", Status.NOT_RUN, reason))
    return plan


def run_plan(plan: Plan, ctx: object) -> list[Result]:
    """Execute a plan. A check that raises becomes FAIL; the rest still run."""
    results: list[Result] = []
    for spec in plan.runnable:
        try:
            out = spec.func(ctx)
        except Exception as exc:  # noqa: BLE001 - one bad check must not hide the others
            results.append(Result(spec.group, spec.id, Status.FAIL,
                                  f"check raised {type(exc).__name__}: {exc}", spec.kind))
            continue
        results.extend(_normalize(spec, out))
    for spec in plan.skipped_slow:
        results.append(Result(spec.group, spec.id, Status.SKIP,
                              "slow check not selected (use --full)", spec.kind))
    results.extend(plan.pending)
    return sorted(results, key=lambda r: (r.group, r.id))


def _normalize(spec: CheckSpec, out: object) -> list[Result]:
    items = out if isinstance(out, (list, tuple)) else [out]
    normalized: list[Result] = []
    for item in items:
        if isinstance(item, Result):
            normalized.append(item)
        else:
            normalized.append(Result(spec.group, spec.id, Status.FAIL,
                                     f"check returned {type(item).__name__}, expected Result", spec.kind))
    if not normalized:
        normalized.append(Result(spec.group, spec.id, Status.FAIL, "check returned no result", spec.kind))
    return normalized


def tally(results: Sequence[Result]) -> dict[str, int]:
    counts = {status.value: 0 for status in STATUS_ORDER}
    for result in results:
        counts[result.status.value] += 1
    return counts


def completed_count(results: Sequence[Result]) -> int:
    return sum(1 for r in results if r.status in COMPLETED)


def progress_bar(completed: int, planned: int, width: int = BAR_WIDTH) -> str:
    if planned <= 0:
        return "[" + "-" * width + "]"
    filled = min(width, completed * width // planned)
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def exit_code(results: Sequence[Result]) -> int:
    counts = tally(results)
    if counts[Status.FAIL.value]:
        return EXIT_FAIL
    if counts[Status.INCOMPLETE.value] or counts[Status.NOT_RUN.value]:
        return EXIT_USAGE_OR_INCOMPLETE
    return EXIT_OK


def summary_dict(results: Sequence[Result], planned: int) -> dict:
    counts = tally(results)
    return {
        "completed": completed_count(results),
        "counts": counts,
        "planned": planned,
        "total": len(results),
    }


def format_human(results: Sequence[Result], planned: int, notes: Sequence[str] = ()) -> str:
    lines = [f"[{r.status.value}] {r.full_id}" + (f" - {r.detail}" if r.detail else "")
             + (" (heuristic)" if r.kind == KIND_HEURISTIC else "")
             for r in results]
    counts = tally(results)
    done = completed_count(results)
    lines.append(f"{progress_bar(done, planned)} {done}/{planned} completed | "
                 + ", ".join(f"{s.value} {counts[s.value]}" for s in STATUS_ORDER))
    lines.extend(notes)
    return "\n".join(lines)


def format_json(results: Sequence[Result], planned: int, extra: dict | None = None) -> str:
    payload = {
        "results": [r.to_dict() for r in results],
        "summary": summary_dict(results, planned),
    }
    if extra:
        payload.update(extra)
    return json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True)
