"""Group "memory": runs .claude/scripts/verify_memory.py against a COPY of the memory database (Step 3.5).

The live .claude/memory/memory.db is only ever read (byte copy); the verifier is pointed at the copy
via TNMEM_DB. Everything generated lives under out/verify/memory/ and is removed afterwards.

  * a -wal/-shm/-journal sidecar beside the source means the database may be mid-write: SKIP;
  * a failed copy, missing script, launch failure or timeout: INCOMPLETE (not verified);
  * the source is hashed before and after; any change is a FAIL (the original must stay byte-identical);
  * verifier output is redacted and capped at 4 KB, and shown only on FAIL.
A pass means the verifier found the copied database internally consistent; nothing more.
"""
from __future__ import annotations

import hashlib
import os
import shutil

import core
from checks_server import MAX_OUTPUT_CHARS, limit_output  # noqa: F401  (shared output rules)
from core import Registry, Result, Status

GROUP = "memory"
ID = "verify"
SIDECAR_SUFFIXES = ("-wal", "-shm", "-journal")
DB_PARTS = (".claude", "memory", "memory.db")
SCRIPT_PARTS = (".claude", "scripts", "verify_memory.py")
WORK_PARTS = ("memory",)  # under out/verify
MEMORY_DIR_PARTS = (".claude", "memory")
LINE_LIMIT_ID = "line_limit"
MAX_MEMORY_LINES = 200  # CLAUDE.md: every memory Markdown file stays at or under 200 lines


def _sha256(path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def build_env(work_dir, db_copy, out_dir, base=None) -> dict:
    source = os.environ if base is None else base
    env = {k: v for k, v in source.items()
           if not k.upper().startswith(("TASKNEST_", "TNMEM_"))}
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(out_dir / "pycache")
    env["TNMEM_DB"] = str(db_copy)
    env["TNMEM_CLAUDE_DIR"] = str(work_dir)            # keep any derived default path inside out/verify
    env["TNMEM_LOG"] = str(work_dir / "usage_log.jsonl")
    return env


def _res(status: Status, detail: str) -> Result:
    return Result(GROUP, ID, status, detail)


def check_memory(ctx):
    source = ctx.path(*DB_PARTS)
    script = ctx.path(*SCRIPT_PARTS)
    if not script.is_file():
        return _res(Status.INCOMPLETE, ".claude/scripts/verify_memory.py not found; memory NOT verified")
    if not source.is_file():
        return _res(Status.SKIP, "no memory database at .claude/memory/memory.db")
    present = [s for s in SIDECAR_SUFFIXES if source.with_name(source.name + s).exists()]
    if present:
        return _res(Status.SKIP, f"memory.db has {'/'.join(present)} sidecar (may be mid-write); not copied or verified")

    work = ctx.out_dir.joinpath(*WORK_PARTS)
    copy = work / "memory.db"
    try:
        before = _sha256(source)
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        with open(source, "rb") as src, open(copy, "wb") as dst:
            shutil.copyfileobj(src, dst)
        if _sha256(copy) != before:
            shutil.rmtree(work, ignore_errors=True)
            return _res(Status.INCOMPLETE, "copy differs from the source (database changing?); memory NOT verified")
    except OSError as exc:
        shutil.rmtree(work, ignore_errors=True)
        return _res(Status.INCOMPLETE,
                    f"could not copy memory.db to out/verify/memory ({type(exc).__name__}); memory NOT verified")

    outcome = core.run_allowed("memory.verify", ctx.root, build_env(work, copy, ctx.out_dir))
    if outcome.timed_out:
        timeout = core.ALLOWED_COMMANDS["memory.verify"].timeout
        return _finish(source, before, work, Status.INCOMPLETE,
                       f"verify_memory.py timed out after {timeout:g}s; memory NOT verified")
    if outcome.launch_error:
        return _finish(source, before, work, Status.INCOMPLETE,
                       f"verify_memory.py could not be launched ({outcome.launch_error}); memory NOT verified")

    output = outcome.output
    if outcome.returncode == 0:
        return _finish(source, before, work, Status.PASS,
                       "verify_memory.py exited 0 on a copy of memory.db (output not shown)")
    shown = limit_output(output)
    detail = f"verify_memory.py exited {outcome.returncode} on a copy of memory.db" + (f"\n{shown}" if shown else "")
    return _finish(source, before, work, Status.FAIL, detail)


def _finish(source, before, work, status: Status, detail: str):
    """Remove the generated copy, then confirm the original is byte-identical."""
    shutil.rmtree(work, ignore_errors=True)
    try:
        after = _sha256(source)
    except OSError as exc:
        return _res(Status.FAIL, f"original memory.db unreadable after verification ({type(exc).__name__})")
    if after != before:
        return _res(Status.FAIL, "original memory.db changed during verification (hash mismatch)")
    return _res(status, detail)


def check_line_limit(ctx):
    """Every .claude/memory/*.md file must have at most MAX_MEMORY_LINES lines (read-only)."""
    directory = ctx.path(*MEMORY_DIR_PARTS)
    if not directory.is_dir():
        return Result(GROUP, LINE_LIMIT_ID, Status.SKIP, "no memory directory at .claude/memory")
    try:
        files = sorted(p for p in directory.glob("*.md") if p.is_file())
    except OSError as exc:
        return Result(GROUP, LINE_LIMIT_ID, Status.INCOMPLETE,
                      f"cannot list .claude/memory ({type(exc).__name__}); line limit NOT verified")
    if not files:
        return Result(GROUP, LINE_LIMIT_ID, Status.SKIP, "no .md files in .claude/memory")
    over, unreadable = [], []
    for path in files:
        try:
            lines = len(path.read_bytes().decode("utf-8", errors="replace").splitlines())
        except OSError:
            unreadable.append(path.name)
            continue
        if lines > MAX_MEMORY_LINES:
            over.append(f"{path.name} ({lines} lines)")
    if over:
        return Result(GROUP, LINE_LIMIT_ID, Status.FAIL,
                      f"over the {MAX_MEMORY_LINES}-line limit: " + ", ".join(over)
                      + ("; unreadable: " + ", ".join(unreadable) if unreadable else ""))
    if unreadable:
        return Result(GROUP, LINE_LIMIT_ID, Status.INCOMPLETE,
                      "unreadable, line limit NOT verified: " + ", ".join(unreadable))
    return Result(GROUP, LINE_LIMIT_ID, Status.PASS,
                  f"{len(files)} memory file(s) are within {MAX_MEMORY_LINES} lines")


def register(registry: Registry) -> None:
    registry.register(GROUP, ID, check_memory)
    registry.register(GROUP, LINE_LIMIT_ID, check_line_limit)
