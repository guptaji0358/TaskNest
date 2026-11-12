"""Shared test setup. Keeps bytecode out of the project and builds fake repo roots."""
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ under tools/verify/

VERIFY_DIR = Path(__file__).resolve().parents[1]
if str(VERIFY_DIR) not in sys.path:
    sys.path.insert(0, str(VERIFY_DIR))

import pytest  # noqa: E402


@pytest.fixture
def fake_repo(tmp_path):
    """A minimal directory that satisfies the root markers (system temp, not the project)."""
    root = tmp_path / "repo"
    (root / ".claude").mkdir(parents=True)
    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.20)\n", encoding="utf-8")
    (root / ".claude" / "CLAUDE.md").write_text("# fixture\n", encoding="utf-8")
    return root
