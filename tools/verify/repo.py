"""Repository path discovery. The only module that knows where project files live.

Step 3.1: root discovery and the generated-output location only. Source-of-truth
readers (OpenAPI, conformance, schema version, ...) arrive with their check groups.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

# Both must exist for a directory to count as the repository root (approved markers).
ROOT_MARKERS = ("CMakeLists.txt", ".claude/CLAUDE.md")

# Everything this toolkit generates goes here (git-ignored via `out/`).
OUT_SUBDIR = ("out", "verify")


class RepoNotFound(RuntimeError):
    """No directory with the root markers was found."""


def is_root(path: Path) -> bool:
    return all((path / marker).is_file() for marker in ROOT_MARKERS)


def find_root(start: Path | str) -> Path:
    """Walk up from `start` (a file or directory) to the first directory with all markers."""
    current = Path(start).resolve()
    if current.is_file():
        current = current.parent
    if not current.exists():
        raise RepoNotFound(f"start path does not exist: {current}")
    for candidate in (current, *current.parents):
        if is_root(candidate):
            return candidate
    raise RepoNotFound(f"no repository root (markers: {', '.join(ROOT_MARKERS)}) at or above {current}")


@dataclass(frozen=True)
class Ctx:
    """Context handed to every check. Paths are resolved once, here."""
    root: Path
    milestone: str = ""

    @property
    def out_dir(self) -> Path:
        """Generated-output directory. Never created by this module."""
        return self.root.joinpath(*OUT_SUBDIR)

    def path(self, *parts: str) -> Path:
        return self.root.joinpath(*parts)
