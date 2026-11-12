"""Read-only Git access for the verification toolkit (Step 3.2).

Only the exact argument lists in ALLOWED can run. There is no shell, the working
directory is the repository root, optional locks and fsmonitor are disabled, and
every call has a timeout. Anything else (unknown commands, extra or altered
arguments, every mutation) raises GitRejected before a process is started.

Raw stdout is kept on the result for later checks but is excluded from repr();
stderr and error text are passed through redact(), so no secret-looking value
reaches a report by accident.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

DEFAULT_TIMEOUT = 30.0  # seconds
MAX_OUTPUT_BYTES = 32 * 1024 * 1024

# Fixed prefix on every call: no fsmonitor daemon or hook, no pager.
FIXED_PREFIX = ("--no-pager", "-c", "core.fsmonitor=false")

# The complete allowlist: name -> exact arguments after the fixed prefix.
ALLOWED: dict[str, tuple[str, ...]] = {
    "ls-files": ("ls-files", "-z"),
    "staged-names": ("diff", "--cached", "--name-only", "-z"),
    "status": ("status", "--porcelain=v1", "-z", "--untracked-files=all"),
    "head": ("rev-parse", "HEAD"),
    "branch": ("branch", "--show-current"),
    "staged-diff": ("diff", "--cached", "--unified=0", "--no-color", "--no-ext-diff"),
}
_ALLOWED_ARGS = {args: name for name, args in ALLOWED.items()}

# Only used to give a clearer rejection message; the allowlist is what enforces safety.
MUTATING = frozenset({
    "add", "commit", "reset", "checkout", "switch", "restore", "rm", "mv", "clean", "stash",
    "merge", "rebase", "cherry-pick", "revert", "pull", "push", "fetch", "clone", "init",
    "tag", "branch-delete", "config", "gc", "prune", "update-index", "apply", "am", "worktree",
    "submodule", "remote", "notes", "filter-branch", "reflog", "symbolic-ref", "update-ref",
})

REDACTED = "[REDACTED]"

# (pattern, replacement); group references keep the surrounding key so reports stay readable.
_SECRET_PATTERNS = (
    (re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?(?:-----END [A-Z ]*PRIVATE KEY-----|$)", re.S), REDACTED),
    (re.compile(r"\b(?:ghp|gho|ghu|ghs|ghr|github_pat)_[A-Za-z0-9_]{16,}"), REDACTED),
    (re.compile(r"\bAKIA[0-9A-Z]{16}\b"), REDACTED),
    (re.compile(r"\bBearer\s+[A-Za-z0-9._~+/=-]{8,}", re.I), "Bearer " + REDACTED),
    (re.compile(r"(?i)\b(password|passwd|secret|token|api[_-]?key|authorization)(\s*[:=]\s*)\S+"),
     r"\1\2" + REDACTED),
    (re.compile(r"(://[^/\s:@]+:)[^@\s/]+(@)"), r"\1" + REDACTED + r"\2"),
)


def redact(text: str) -> str:
    """Mask secret-looking values. Best effort; used for every string that may be shown."""
    for pattern, replacement in _SECRET_PATTERNS:
        text = pattern.sub(replacement, text)
    return text


class GitRejected(ValueError):
    """The requested command is not on the allowlist."""


class GitUnavailable(RuntimeError):
    """git cannot be found or the repository directory is unusable."""


@dataclass(frozen=True)
class GitResult:
    name: str
    args: tuple[str, ...]
    returncode: int | None
    stdout: bytes = field(default=b"", repr=False)  # raw, for checks; never printed
    stderr: str = ""                                # redacted
    timed_out: bool = False
    error: str = ""                                 # redacted; launch/limit problems

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and not self.timed_out and not self.error

    @property
    def text(self) -> str:
        return self.stdout.decode("utf-8", errors="replace")

    def describe(self) -> str:
        """Short, safe one-line summary for report details."""
        if self.timed_out:
            return f"git {self.name} timed out"
        if self.error:
            return f"git {self.name} failed: {self.error}"
        if self.returncode != 0:
            return f"git {self.name} exited {self.returncode}: {self.stderr.strip()[:200]}"
        return f"git {self.name} ok"


def classify(args: Sequence[str]) -> str:
    """Return the allowlist name for exact `args`, else raise GitRejected."""
    key = tuple(args)
    name = _ALLOWED_ARGS.get(key)
    if name is not None:
        return name
    first = key[0] if key else ""
    if first in MUTATING:
        raise GitRejected(f"mutating git command is not allowed: {first}")
    raise GitRejected(f"git command is not on the allowlist: {' '.join(key[:3])}")


def git_executable() -> str | None:
    return shutil.which("git")


def _environment() -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("GIT_")}
    env.update({
        "GIT_OPTIONAL_LOCKS": "0",
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_PAGER": "cat",
        "GIT_CONFIG_NOSYSTEM": "1",
        "LC_ALL": "C",
    })
    return env


def run_git(root: Path | str, args: Sequence[str], *, timeout: float = DEFAULT_TIMEOUT,
            _exe: Sequence[str] | None = None) -> GitResult:
    """Run one allowlisted command in `root`. `_exe` replaces the git binary (tests only)."""
    name = classify(args)
    cwd = Path(root)
    if not cwd.is_dir():
        raise GitUnavailable(f"repository directory does not exist: {cwd}")
    if _exe is None:
        exe = git_executable()
        if exe is None:
            raise GitUnavailable("git executable not found on PATH")
        prefix: list[str] = [exe]
    else:
        prefix = list(_exe)
    argv = [*prefix, *FIXED_PREFIX, *args]
    try:
        proc = subprocess.run(argv, cwd=str(cwd), env=_environment(), shell=False,
                              capture_output=True, timeout=timeout, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return GitResult(name, tuple(args), None, timed_out=True)
    except OSError as exc:
        return GitResult(name, tuple(args), None, error=redact(f"{type(exc).__name__}: {exc}"))
    if len(proc.stdout) > MAX_OUTPUT_BYTES:
        return GitResult(name, tuple(args), proc.returncode, error="output exceeds size limit")
    return GitResult(name, tuple(args), proc.returncode, proc.stdout,
                     redact(proc.stderr.decode("utf-8", errors="replace")))


# --- structured helpers ---------------------------------------------------------------------

def _split_z(data: bytes) -> list[str]:
    return [p.decode("utf-8", errors="replace") for p in data.split(b"\0") if p]


def ls_files(root, **kw) -> GitResult:
    return run_git(root, ALLOWED["ls-files"], **kw)


def staged_names(root, **kw) -> GitResult:
    return run_git(root, ALLOWED["staged-names"], **kw)


def status(root, **kw) -> GitResult:
    return run_git(root, ALLOWED["status"], **kw)


def head(root, **kw) -> GitResult:
    return run_git(root, ALLOWED["head"], **kw)


def current_branch(root, **kw) -> GitResult:
    return run_git(root, ALLOWED["branch"], **kw)


def staged_diff(root, **kw) -> GitResult:
    return run_git(root, ALLOWED["staged-diff"], **kw)


def parse_paths(result: GitResult) -> list[str]:
    """NUL-separated path list (ls-files, diff --name-only)."""
    return _split_z(result.stdout) if result.ok else []


def parse_status(result: GitResult) -> list[tuple[str, str]]:
    """Porcelain v1 -z entries as (XY, path). Rename/copy entries keep the new path;
    the following origin path field is consumed and dropped."""
    if not result.ok:
        return []
    fields = result.stdout.split(b"\0")
    entries: list[tuple[str, str]] = []
    i = 0
    while i < len(fields):
        raw = fields[i]
        i += 1
        if len(raw) < 4:
            continue
        xy = raw[:2].decode("ascii", errors="replace")
        entries.append((xy, raw[3:].decode("utf-8", errors="replace")))
        if "R" in xy or "C" in xy:
            i += 1
    return entries


def parse_single_line(result: GitResult) -> str:
    return result.text.strip() if result.ok else ""
