"""Whole-tree filesystem snapshots for write-isolation tests.

Unlike a size/mtime listing that skips `.git` and `out/`, this walks everything and
records content hashes, so created, modified and deleted files and directories are
all detected anywhere, including `.git/` and `out/`.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path

# Writes are permitted only here (repo-relative, POSIX style). The system temp
# directory is outside the tree and therefore never appears in a snapshot.
ALLOWED_PREFIXES = ("out/verify",)


def snapshot(root: Path) -> dict[str, str]:
    """Map repo-relative POSIX path -> 'dir' or 'file:<size>:<sha256>'."""
    root = Path(root)
    state: dict[str, str] = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        base = Path(dirpath)
        for name in dirnames:
            state[(base / name).relative_to(root).as_posix()] = "dir"
        for name in sorted(filenames):
            path = base / name
            rel = path.relative_to(root).as_posix()
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            state[rel] = f"file:{path.stat().st_size}:{digest}"
    return state


def is_allowed(rel: str, allowed: tuple[str, ...] = ALLOWED_PREFIXES) -> bool:
    return any(rel == prefix or rel.startswith(prefix + "/") for prefix in allowed)


def diff(before: dict[str, str], after: dict[str, str],
         allowed: tuple[str, ...] = ALLOWED_PREFIXES) -> dict[str, list[str]]:
    """Unexpected changes only: anything under an allowed prefix is ignored."""
    created = sorted(p for p in after.keys() - before.keys() if not is_allowed(p, allowed))
    deleted = sorted(p for p in before.keys() - after.keys() if not is_allowed(p, allowed))
    modified = sorted(p for p in before.keys() & after.keys()
                      if before[p] != after[p] and not is_allowed(p, allowed))
    return {"created": created, "deleted": deleted, "modified": modified}


def has_changes(result: dict[str, list[str]]) -> bool:
    return any(result.values())
