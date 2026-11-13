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
import subprocess
import sys

from checks_server import MAX_OUTPUT_CHARS, limit_output  # noqa: F401  (shared output rules)
from core import Registry, Result, Status

GROUP = "memory"
ID = "verify"
TIMEOUT_SECONDS = 120
SIDECAR_SUFFIXES = ("-wal", "-shm", "-journal")
DB_PARTS = (".claude", "memory", "memory.db")
SCRIPT_PARTS = (".claude", "scripts", "verify_memory.py")
WORK_PARTS = ("memory",)  # under out/verify


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

    try:
        proc = subprocess.run([sys.executable, "-B", str(script)], cwd=str(ctx.root),
                              env=build_env(work, copy, ctx.out_dir), shell=False, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return _finish(source, before, work, Status.INCOMPLETE,
                       f"verify_memory.py timed out after {TIMEOUT_SECONDS}s; memory NOT verified")
    except OSError as exc:
        return _finish(source, before, work, Status.INCOMPLETE,
                       f"verify_memory.py could not be launched ({type(exc).__name__}); memory NOT verified")

    output = proc.stdout.decode("utf-8", "replace")
    if proc.returncode == 0:
        return _finish(source, before, work, Status.PASS,
                       "verify_memory.py exited 0 on a copy of memory.db (output not shown)")
    shown = limit_output(output)
    detail = f"verify_memory.py exited {proc.returncode} on a copy of memory.db" + (f"\n{shown}" if shown else "")
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


def register(registry: Registry) -> None:
    registry.register(GROUP, ID, check_memory)
