"""Group "server": runs the existing server checker (server/check.py) as a subprocess (Step 3.5).

Fixed command via core.run_allowed("server.check"): [sys.executable, "-B", "<root>/server/check.py"],
no shell, cwd = repository root.
The checker's output is never parsed. The exit code alone decides the status
(0 PASS, 1 FAIL, 2 INCOMPLETE, anything else FAIL). Captured output (stdout and stderr merged,
redacted, capped at 4 KB) is shown only for FAIL and INCOMPLETE.

The check is registered as slow: it runs only with --full (it includes pytest); otherwise the
core runner reports SKIP. This module never modifies server/ and never installs anything.
Note: server/check.py itself redirects its own caches under out/server/ (its documented behavior).
"""
from __future__ import annotations

import os

import gitro
import core
from core import Registry, Result, Status

GROUP = "server"
ID = "check"
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
    outcome = core.run_allowed("server.check", ctx.root, build_env(ctx.out_dir))
    timeout = core.ALLOWED_COMMANDS["server.check"].timeout
    if outcome.timed_out:
        return Result(GROUP, ID, Status.INCOMPLETE,
                      _with_output(f"server/check.py timed out after {timeout:g}s; server NOT verified",
                                   outcome.output))
    if outcome.launch_error:
        return Result(GROUP, ID, Status.INCOMPLETE,
                      f"server/check.py could not be launched ({outcome.launch_error}); server NOT verified")
    output = outcome.output
    code = outcome.returncode
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
