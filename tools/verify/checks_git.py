"""Group "git": repository facts, explicit baseline, secret scan, forbidden paths, milestone scope (Step 3.4).

All Git access goes through the `gitro` allowlist (read-only, no shell). The only file this
module ever writes is `out/verify/baseline.json`, and only from `save_baseline`, which the CLI
calls for an explicit `--baseline-save`. Ordinary verification never creates or refreshes it.

Without a baseline the checks report facts and issue no scope verdict. Secret scanning keeps
matched text in memory only: reports name the file, line and pattern, never the value.
Heuristic checks (pattern scans) are registered with kind="heuristic".
"""
from __future__ import annotations

import fnmatch
import hashlib
import json
import os
import re
from pathlib import Path

import gitro
from core import KIND_DETERMINISTIC, KIND_HEURISTIC, Registry, Result, Status

GROUP = "git"
BASELINE_FORMAT = "tasknest-verify-baseline/1"
MILESTONES_FORMAT = "tasknest-verify-milestones/1"
BASELINE_RELPATH = ("out", "verify", "baseline.json")
MILESTONES_RELPATH = ("tools", "verify", "milestones.json")
TOOL_OUTPUT_PREFIX = "out/verify/"  # the toolkit's own output is never part of a task's changes
MILESTONE_ID_RE = re.compile(r"^M\d+(\.\d+)*$")
HEAD_RE = re.compile(r"^[0-9a-f]{40}([0-9a-f]{24})?$")
MAX_LISTED = 8

# Paths that must never be tracked (CLAUDE.md: no *.db, *.sqlite*, executables, objects or generated files).
FORBIDDEN_TRACKED = ("*.db", "*.sqlite", "*.sqlite3", "*.sqlite-*", "*.db-wal", "*.db-shm", "*.exe", "*.dll",
                     "*.o", "*.obj", "*.pyc", "out/**", "**/__pycache__/**", "*.bak")


def _res(id: str, status: Status, detail: str = "", kind: str = KIND_DETERMINISTIC) -> Result:
    return Result(GROUP, id, status, detail, kind)


def _listed(items) -> str:
    items = sorted(items)
    more = f" (+{len(items) - MAX_LISTED} more)" if len(items) > MAX_LISTED else ""
    return ", ".join(items[:MAX_LISTED]) + more


def path_matches(path: str, pattern: str) -> bool:
    """`dir/**` is a prefix rule, `**/x/**` matches `x` anywhere, a wildcard-only-name like `*.ext`
    matches at any depth, a bare name matches only at the root; anything else is fnmatch."""
    if pattern.startswith("**/") and pattern.endswith("/**"):
        segment = pattern[3:-3]
        return f"/{segment}/" in f"/{path}"
    if pattern.endswith("/**"):
        return path.startswith(pattern[:-3] + "/")
    if "/" not in pattern and any(c in pattern for c in "*?["):
        return fnmatch.fnmatchcase(path.rsplit("/", 1)[-1], pattern)  # `*.ext` at any depth
    if "/" not in pattern:
        return path == pattern  # a bare file name means that file at the root only
    return fnmatch.fnmatchcase(path, pattern)


# --- repository state ------------------------------------------------------------------------

class GitState:
    """What git reports right now. `error` is set (and nothing else is trusted) if git failed."""

    def __init__(self, root: Path):
        self.root = root
        self.error = ""
        self.head = ""
        self.branch = ""
        self.entries: list[tuple[str, str]] = []      # without the toolkit's own out/verify/ files
        self.all_entries: list[tuple[str, str]] = []  # everything git reports, tracked changes and untracked
        self.tracked: list[str] = []
        self.staged: list[str] = []
        try:
            results = {"status": gitro.status(root), "head": gitro.head(root), "ls": gitro.ls_files(root),
                       "staged": gitro.staged_names(root), "branch": gitro.current_branch(root)}
        except (gitro.GitUnavailable, gitro.GitRejected) as exc:
            self.error = str(exc)
            return
        for key in ("status", "head", "ls", "staged"):
            if not results[key].ok:
                self.error = results[key].describe()
                return
        self.head = gitro.parse_single_line(results["head"])
        self.branch = gitro.parse_single_line(results["branch"])  # informational only
        self.all_entries = gitro.parse_status(results["status"])
        self.entries = [(xy, p) for xy, p in self.all_entries if not p.startswith(TOOL_OUTPUT_PREFIX)]
        self.tracked = gitro.parse_paths(results["ls"])
        self.staged = gitro.parse_paths(results["staged"])

    def categories(self) -> dict[str, list[str]]:
        out = {"staged": [], "modified": [], "untracked": []}
        for xy, path in self.entries:
            if xy == "??":
                out["untracked"].append(path)
                continue
            if xy[0] not in " ?!":
                out["staged"].append(path)
            if xy[1] not in " ?!":
                out["modified"].append(path)
        return {k: sorted(v) for k, v in out.items()}


def file_sha256(root: Path, rel: str) -> str | None:
    """SHA-256 of a regular file's bytes; None if it is missing (deleted) or not a regular file."""
    path = root / rel
    try:
        if path.is_symlink() or not path.is_file():
            return None
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def snapshot_entries(state: GitState) -> dict[str, dict]:
    return {path: {"sha256": file_sha256(state.root, path), "xy": xy} for xy, path in sorted(state.entries, key=lambda e: e[1])}


# --- baseline (explicit only) ----------------------------------------------------------------

def baseline_path(root: Path) -> Path:
    return root.joinpath(*BASELINE_RELPATH)


def _is_link(path: Path) -> bool:
    junction = getattr(path, "is_junction", None)
    return path.is_symlink() or bool(junction and junction())


def _unsafe_baseline_component(root: Path, target: Path) -> str:
    """Reason string if any path component is a link (incl. dangling) or the wrong kind, else "".

    Checked before anything is created. Residual limitation: this is check-then-write, so a link
    swapped in between the check and the write is not detected (no O_NOFOLLOW-style handles).
    """
    out, verify = root / "out", target.parent
    for path, must_be_dir in ((out, True), (verify, True), (target, False),
                              (target.with_name("baseline.json.tmp"), False)):
        rel = path.relative_to(root).as_posix()
        if _is_link(path):
            return f"{rel} is a symlink or junction"
        if os.path.lexists(path):
            if must_be_dir and not path.is_dir():
                return f"{rel} exists but is not a directory"
            if not must_be_dir and not path.is_file():
                return f"{rel} exists but is not a regular file"
    return ""


def save_baseline(root: Path, force: bool = False) -> tuple[bool, str]:
    """Write out/verify/baseline.json. Refuses to overwrite unless `force`. Returns (ok, message)."""
    state = GitState(root)
    if state.error:
        return False, f"cannot read git state: {state.error}"
    target = baseline_path(root)
    unsafe = _unsafe_baseline_component(root, target)
    if unsafe:
        return False, f"refusing to write baseline: {unsafe}"
    try:
        out_dir = target.parent.resolve()
        if not out_dir.is_relative_to((root / "out").resolve()):
            return False, "refusing to write outside out/"
    except (OSError, ValueError):
        return False, "cannot resolve out/verify"
    if target.exists() and not force:
        return False, "out/verify/baseline.json already exists; use --force to overwrite"
    payload = {"entries": snapshot_entries(state), "format": BASELINE_FORMAT, "head": state.head}
    text = json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        temp = target.with_name("baseline.json.tmp")
        temp.write_bytes(text.encode("utf-8"))
        os.replace(temp, target)
    except OSError as exc:
        return False, f"cannot write baseline: {type(exc).__name__}"
    return True, f"baseline saved: out/verify/baseline.json (HEAD {state.head[:12]}, {len(payload['entries'])} paths)"


def load_baseline(root: Path):
    """Returns (baseline dict, None) or (None, reason)."""
    path = baseline_path(root)
    if not path.is_file():
        return None, "no saved baseline (out/verify/baseline.json); run --baseline-save first"
    try:
        data = json.loads(path.read_bytes().decode("utf-8"))
    except (OSError, ValueError):
        return None, "baseline file is unreadable or not valid JSON"
    entries = data.get("entries") if isinstance(data, dict) else None
    if (not isinstance(data, dict) or data.get("format") != BASELINE_FORMAT
            or not isinstance(data.get("head"), str) or not HEAD_RE.match(data["head"])
            or not isinstance(entries, dict)):
        return None, "baseline file is malformed"
    for p, e in entries.items():
        if (not isinstance(e, dict) or not isinstance(e.get("xy"), str)
                or not (e.get("sha256") is None or isinstance(e.get("sha256"), str))):
            return None, f"baseline entry is malformed: {p}"
    return data, None


def compare_baseline(state: GitState, baseline: dict) -> dict[str, list[str]]:
    """Attribute changes since the baseline. Unchanged pre-existing paths are NOT attributed."""
    now = snapshot_entries(state)
    before = baseline["entries"]
    result = {"new": [], "content_changed": [], "pre_existing_unchanged": [], "cleared": []}
    for path, entry in now.items():
        if path not in before:
            result["new"].append(path)
        elif before[path]["sha256"] != entry["sha256"]:
            result["content_changed"].append(path)
        else:
            result["pre_existing_unchanged"].append(path)
    result["cleared"] = [p for p in before if p not in now]
    return {k: sorted(v) for k, v in result.items()}


# --- milestones ------------------------------------------------------------------------------

def _no_dupes(pairs):
    keys = [k for k, _ in pairs]
    dup = {k for k in keys if keys.count(k) > 1}
    if dup:
        raise ValueError(f"duplicate key {sorted(dup)[0]!r}")
    return dict(pairs)


def load_milestones(root: Path):
    """Returns (config, None) or (None, (Status, reason)). A broken config is never a PASS."""
    path = root.joinpath(*MILESTONES_RELPATH)
    if not path.is_file():
        return None, (Status.INCOMPLETE, "tools/verify/milestones.json is missing")
    try:
        data = json.loads(path.read_bytes().decode("utf-8"), object_pairs_hook=_no_dupes)
    except (OSError, ValueError) as exc:
        return None, (Status.FAIL, f"milestones.json is not valid JSON: {type(exc).__name__}")
    problem = _milestone_problem(data)
    if problem:
        return None, (Status.FAIL, f"milestones.json is malformed: {problem}")
    return data, None


def _milestone_problem(data) -> str:
    if not isinstance(data, dict) or data.get("format") != MILESTONES_FORMAT:
        return f"format is not {MILESTONES_FORMAT!r}"
    miles = data.get("milestones")
    if not isinstance(miles, dict) or not miles:
        return "'milestones' is missing or empty"
    if data.get("default") not in miles:
        return "'default' does not name a configured milestone"
    for mid, cfg in miles.items():
        if not MILESTONE_ID_RE.match(mid) or not isinstance(cfg, dict):
            return f"invalid milestone entry {mid!r}"
        if not isinstance(cfg.get("approved"), bool):
            return f"{mid}: 'approved' must be true or false"
        for key in ("allowed_paths", "forbidden_imports"):
            value = cfg.get(key)
            if not isinstance(value, list) or not all(isinstance(v, str) and v for v in value):
                return f"{mid}: '{key}' must be a list of non-empty strings"
        if cfg.get("allowed_paths_status") not in ("proposed", "confirmed"):
            return f"{mid}: 'allowed_paths_status' must be 'proposed' or 'confirmed'"
        if cfg["approved"] and not cfg["allowed_paths"]:
            return f"{mid}: approved but has no allowed_paths"
    return ""


# --- staged-diff secret scan (heuristic; values are never stored or printed) -----------------

SECRET_PATTERNS = (
    ("private_key_block", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("github_token", re.compile(r"\b(?:ghp|gho|ghu|ghs|ghr|github_pat)_[A-Za-z0-9_]{16,}")),
    ("aws_access_key_id", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("bearer_token", re.compile(r"\bBearer\s+[A-Za-z0-9._~+/=-]{16,}", re.I)),
    ("url_credentials", re.compile(r"://[^/\s:@]+:[^@\s/]{3,}@")),
    ("credential_assignment",
     re.compile(r"""(?i)\b(?:password|passwd|secret|token|api[_-]?key)\b["']?\s*[:=]\s*["']?[A-Za-z0-9/+_.~=-]{12,}""")),
)
_PLACEHOLDER_RE = re.compile(r"(?i)(example|placeholder|changeme|dummy|your[_-]|<[^>]+>|\$\{|xxxx)")
_HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")


def scan_diff(diff_text: str) -> list[tuple[str, int, str]]:
    """Findings as (file, line, pattern name), sorted. Only added lines are examined."""
    findings: set[tuple[str, int, str]] = set()
    current, line_no = None, 0
    for raw in diff_text.splitlines():
        if raw.startswith("+++ "):
            name = raw[4:].strip().strip('"')
            current = name[2:] if name.startswith("b/") else None
            continue
        if raw.startswith("--- ") or raw.startswith("diff --git"):
            continue
        hunk = _HUNK_RE.match(raw)
        if hunk:
            line_no = int(hunk.group(1))
            continue
        if raw.startswith("+") and current:
            for name, pattern in SECRET_PATTERNS:
                match = pattern.search(raw[1:])
                if match and not _PLACEHOLDER_RE.search(match.group(0)):
                    findings.add((current, line_no, name))
            line_no += 1
    return sorted(findings)


# --- checks ------------------------------------------------------------------------------------

def _incomplete(id: str, state: GitState, kind: str = KIND_DETERMINISTIC):
    return _res(id, Status.INCOMPLETE, f"git state unavailable: {state.error}", kind)


def check_status_facts(ctx):
    state = GitState(ctx.root)
    if state.error:
        return _incomplete("status_facts", state)
    cats = state.categories()
    detail = (f"tracked {len(state.tracked)}; staged {len(cats['staged'])}; modified {len(cats['modified'])}; "
              f"untracked {len(cats['untracked'])}; HEAD {state.head[:12]}; branch "
              f"{state.branch or '(detached)'} (informational only). Facts only, no verdict.")
    for name in ("staged", "modified", "untracked"):
        if cats[name]:
            detail += f" {name}: {_listed(cats[name])}."
    return _res("status_facts", Status.PASS, detail)


def check_baseline(ctx):
    if not getattr(ctx, "baseline", False):
        return _res("baseline", Status.SKIP, "no --baseline given; changes are facts only, no scope verdict")
    state = GitState(ctx.root)
    if state.error:
        return _incomplete("baseline", state)
    base, why = load_baseline(ctx.root)
    if base is None:
        return _res("baseline", Status.INCOMPLETE, why)
    if base["head"] != state.head:
        return _res("baseline", Status.INCOMPLETE,
                    f"stale baseline: saved at HEAD {base['head'][:12]}, now {state.head[:12]}; "
                    "no attribution made (re-save with --baseline-save --force)")
    diff = compare_baseline(state, base)
    detail = (f"new {len(diff['new'])}; content-changed {len(diff['content_changed'])}; "
              f"pre-existing unchanged {len(diff['pre_existing_unchanged'])} (not attributed); "
              f"cleared since baseline {len(diff['cleared'])}.")
    for name in ("new", "content_changed"):
        if diff[name]:
            detail += f" {name.replace('_', '-')}: {_listed(diff[name])}."
    return _res("baseline", Status.PASS, detail)


def forbidden_pattern_hit(path: str) -> str:
    """The first prohibited-artifact pattern `path` matches, or ''. Paths inside `.git/` are never
    project artifacts. Permitted toolkit output (out/verify/) skips only the generic `out/**` rule;
    it is still flagged if it independently matches another rule (a database, executable, ...)."""
    if path == ".git" or path.startswith(".git/"):
        return ""
    permitted_output = path.startswith(TOOL_OUTPUT_PREFIX)
    for pattern in FORBIDDEN_TRACKED:
        if permitted_output and pattern == "out/**":
            continue
        if path_matches(path, pattern):
            return pattern
    return ""


# Directory names that are build/bytecode output when they are git-ignored (nothing generated may
# live outside out/). A directory holding any tracked or untracked path is left to the path rules.
FORBIDDEN_IGNORED_DIRS = ("build", "bin", "Debug", "Release", "RelWithDebInfo", "MinSizeRel", "x64", "x86",
                          "CMakeFiles", "cmake-build-*", "__pycache__")
# External or vendored trees: never entered, never reported.
SKIPPED_DIRS = frozenset({"node_modules", ".venv", "venv", ".tox", ".mypy_cache", ".pytest_cache"})
MAX_SCAN_ENTRIES = 200_000
MAX_SCAN_DEPTH = 40


def _is_output_dir(name: str) -> bool:
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in FORBIDDEN_IGNORED_DIRS)


def _is_link(entry) -> bool:
    """Symlinks and Windows junctions are links: they are never entered, only judged by their own name."""
    junction = getattr(entry, "is_junction", None)
    return entry.is_symlink() or bool(junction and junction())


def scan_ignored(root: Path, known: set, dirs_with_known: set, max_entries: int = MAX_SCAN_ENTRIES):
    """Walk the repository by path only: file contents are never opened and symlinks are never followed.

    `known` holds every tracked or untracked path git reports; anything else on disk is git-ignored.
    The repository's own `.git/`, external trees (SKIPPED_DIRS) and everything under `out/` except
    `out/verify/` are not entered (`out/` is the designated git-ignored output area).
    Returns (ignored findings as sorted paths, scanned entry count, truncated flag)."""
    findings: set[str] = set()
    scanned = 0
    stack: list[tuple[str, int]] = [("", 0)]
    while stack:
        rel_dir, depth = stack.pop()
        try:
            with os.scandir(root / rel_dir if rel_dir else root) as it:
                entries = sorted(it, key=lambda e: e.name)
        except OSError:
            continue
        for entry in entries:
            scanned += 1
            if scanned > max_entries:
                return sorted(findings), scanned, True
            rel = f"{rel_dir}/{entry.name}" if rel_dir else entry.name
            try:
                is_dir = entry.is_dir(follow_symlinks=False) and not _is_link(entry)
            except OSError:
                continue
            if is_dir:
                if rel == ".git" or entry.name in SKIPPED_DIRS:
                    continue  # the repository's own .git/ and external trees
                if rel == "out":
                    if (root / "out" / "verify").is_dir():
                        stack.append(("out/verify", depth + 2))
                    continue
                if rel not in dirs_with_known and _is_output_dir(entry.name):
                    findings.add(rel + "/")  # flag the directory itself; its contents are not listed
                    continue
                if depth + 1 <= MAX_SCAN_DEPTH:
                    stack.append((rel, depth + 1))
            elif rel not in known and forbidden_pattern_hit(rel):
                findings.add(rel)  # regular files and symlinks, judged by name only
    return sorted(findings), scanned, False


def check_forbidden_paths(ctx):
    state = GitState(ctx.root)
    if state.error:
        return _incomplete("forbidden_paths", state)
    untracked_all = sorted(p for xy, p in state.all_entries if xy == "??")
    tracked = sorted(p for p in state.tracked if forbidden_pattern_hit(p))
    untracked = sorted(p for p in untracked_all if forbidden_pattern_hit(p))
    known = set(state.tracked) | set(untracked_all)
    dirs_with_known = {"/".join(p.split("/")[:i]) for p in known for i in range(1, p.count("/") + 1)}
    ignored, scanned, truncated = scan_ignored(ctx.root, known, dirs_with_known, MAX_SCAN_ENTRIES)
    if tracked or untracked or ignored:
        parts = []
        for label, items in (("tracked", tracked), ("untracked", untracked), ("ignored", ignored)):
            if items:
                parts.append(f"{len(items)} {label}: {_listed(items)}")
        note = f" (ignored-file scan stopped after {scanned} entries)" if truncated else ""
        return _res("forbidden_paths", Status.FAIL, "forbidden path(s) - " + "; ".join(parts) + note)
    if truncated:
        return _res("forbidden_paths", Status.INCOMPLETE,
                    f"ignored-file scan stopped after {scanned} entries; tracked and untracked paths are clean "
                    "but ignored paths were not fully inspected")
    return _res("forbidden_paths", Status.PASS,
                f"none of {len(state.tracked)} tracked, {len(untracked_all)} untracked or the git-ignored paths "
                f"({scanned} filesystem entries scanned by path only) match the forbidden patterns (databases, "
                "executables, objects, bytecode, backups, build output); out/ is not entered except out/verify/")


def check_staged_secrets(ctx):
    kind = KIND_HEURISTIC
    try:
        result = gitro.staged_diff(ctx.root)
    except (gitro.GitUnavailable, gitro.GitRejected) as exc:
        return _res("staged_secrets", Status.INCOMPLETE, f"git state unavailable: {exc}", kind)
    if not result.ok:
        return _res("staged_secrets", Status.INCOMPLETE, result.describe(), kind)
    findings = scan_diff(result.text)
    if findings:
        shown = [f"{f}:{n} [{p}]" for f, n, p in findings[:MAX_LISTED]]
        more = f" (+{len(findings) - MAX_LISTED} more)" if len(findings) > MAX_LISTED else ""
        return _res("staged_secrets", Status.FAIL,
                    f"{len(findings)} possible secret(s) in staged additions (values not shown): "
                    + ", ".join(shown) + more, kind)
    return _res("staged_secrets", Status.PASS,
                "no staged addition matched a secret pattern (pattern scan; not proof of absence)", kind)


def check_milestone_scope(ctx):
    config, problem = load_milestones(ctx.root)
    if config is None:
        return _res("milestone_scope", problem[0], problem[1])
    mid = ctx.milestone or config["default"]
    cfg = config["milestones"].get(mid)
    if cfg is None:
        return _res("milestone_scope", Status.INCOMPLETE, f"milestone {mid} is not configured; no scope verdict")
    if not cfg["approved"]:
        return _res("milestone_scope", Status.INCOMPLETE,
                    f"milestone {mid} is not approved in milestones.json; no scope verdict "
                    "(branch names never establish approval)")
    if cfg["allowed_paths_status"] != "confirmed":
        return _res("milestone_scope", Status.INCOMPLETE,
                    f"milestone {mid} allowed paths are PROPOSED, not owner-confirmed ({_listed(cfg['allowed_paths'])}); "
                    "no scope verdict until the owner confirms the exact list")
    if not getattr(ctx, "baseline", False):
        return _res("milestone_scope", Status.SKIP,
                    f"milestone {mid} is approved, but without --baseline changes cannot be attributed to the "
                    "task; no scope verdict")
    state = GitState(ctx.root)
    if state.error:
        return _incomplete("milestone_scope", state)
    base, why = load_baseline(ctx.root)
    if base is None:
        return _res("milestone_scope", Status.INCOMPLETE, why)
    if base["head"] != state.head:
        return _res("milestone_scope", Status.INCOMPLETE, "stale baseline (HEAD moved); no scope verdict")
    diff = compare_baseline(state, base)
    attributed = diff["new"] + diff["content_changed"]
    outside = [p for p in attributed if not any(path_matches(p, pat) for pat in cfg["allowed_paths"])]
    if outside:
        return _res("milestone_scope", Status.FAIL,
                    f"{len(outside)} changed path(s) outside milestone {mid}: {_listed(outside)}")
    return _res("milestone_scope", Status.PASS,
                f"{len(attributed)} attributed path(s) are within milestone {mid} allowed paths "
                "(path scope only; says nothing about correctness)")


CHECKS = (
    ("baseline", check_baseline, KIND_DETERMINISTIC),
    ("forbidden_paths", check_forbidden_paths, KIND_DETERMINISTIC),
    ("milestone_scope", check_milestone_scope, KIND_DETERMINISTIC),
    ("staged_secrets", check_staged_secrets, KIND_HEURISTIC),
    ("status_facts", check_status_facts, KIND_DETERMINISTIC),
)


def register(registry: Registry) -> None:
    for id, func, kind in CHECKS:
        registry.register(GROUP, id, func, kind=kind)
