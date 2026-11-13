"""Group "server": runs the existing server checker (server/check.py) as a subprocess (Step 3.5).

Fixed command, no shell: [sys.executable, "-B", "<root>/server/check.py"], cwd = repository root.
The checker's output is never parsed. The exit code alone decides the status
(0 PASS, 1 FAIL, 2 INCOMPLETE, anything else FAIL). Captured output (stdout and stderr merged,
redacted, capped at 4 KB) is shown only for FAIL and INCOMPLETE.

The check is registered as slow: it runs only with --full (it includes pytest); otherwise the
core runner reports SKIP. This module never modifies server/ and never installs anything.
Note: server/check.py itself redirects its own caches under out/server/ (its documented behavior).
"""
from __future__ import annotations

import os
import subprocess
import sys

import gitro
from core import Registry, Result, Status

GROUP = "server"
ID = "check"
TIMEOUT_SECONDS = 900
MAX_OUTPUT_CHARS = 4096  # displayed output limit (4 KB)
TRUNCATION_MARK = "\n[output truncated]"
EXIT_STATUS = {0: Status.PASS, 1: Status.FAIL, 2: Status.INCOMPLETE}


def build_env(out_dir, base=None) -> dict:
    """Child environment: no TASKNEST_* variables, bytecode redirected under out/verify/pycache."""
    source = os.environ if base is None else base
    env = {k: v for k, v in source.items() if not k.upper().startswith("TASKNEST_")}
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(out_dir / "pycache")
    return env


def limit_output(text: str) -> str:
    """Redact secret-looking values, then cap at 4 KB."""
    text = gitro.redact(text.strip())
    if len(text) > MAX_OUTPUT_CHARS:
        text = text[:MAX_OUTPUT_CHARS] + TRUNCATION_MARK
    return text


def _with_output(summary: str, output: str) -> str:
    shown = limit_output(output)
    return f"{summary}\n{shown}" if shown else summary


def check_server(ctx):
    script = ctx.path("server", "check.py")
    if not script.is_file():
        return Result(GROUP, ID, Status.INCOMPLETE, "server/check.py not found; server NOT verified")
    command = [sys.executable, "-B", str(script)]
    try:
        proc = subprocess.run(command, cwd=str(ctx.root), env=build_env(ctx.out_dir), shell=False,
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"").decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        return Result(GROUP, ID, Status.INCOMPLETE,
                      _with_output(f"server/check.py timed out after {TIMEOUT_SECONDS}s; server NOT verified", out))
    except OSError as exc:
        return Result(GROUP, ID, Status.INCOMPLETE,
                      f"server/check.py could not be launched ({type(exc).__name__}); server NOT verified")
    output = proc.stdout.decode("utf-8", "replace")
    code = proc.returncode
    status = EXIT_STATUS.get(code, Status.FAIL)
    if status is Status.PASS:
        return Result(GROUP, ID, status, "server/check.py exited 0 (output not parsed or shown)")
    if status is Status.INCOMPLETE:
        return Result(GROUP, ID, status,
                      _with_output("server/check.py exited 2 (required packages missing or checker incomplete); "
                                   "server NOT verified", output))
    if code == 1:
        return Result(GROUP, ID, status, _with_output("server/check.py exited 1", output))
    return Result(GROUP, ID, status, _with_output(f"server/check.py exited with unexpected code {code}", output))


def register(registry: Registry) -> None:
    registry.register(GROUP, ID, check_server, slow=True)
