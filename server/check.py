"""Reusable M2.1 validation: one command, PASS/FAIL per step, nonzero exit on failure.

    python server/check.py

Never installs anything. Runs from any directory. Bytecode and pytest caches
are redirected under out/server/ (nothing generated outside out/).
Exit codes: 0 all passed, 1 a check failed, 2 required packages are missing.
"""
from __future__ import annotations

import ast
import importlib.util
import os
import re
import subprocess
import sys
from pathlib import Path

SERVER = Path(__file__).resolve().parent
ROOT = SERVER.parent
OUT = ROOT / "out" / "server"
REQUIRED = ["fastapi", "starlette", "uvicorn", "pydantic", "httpx", "pytest"]
CONTRACT_PATHS = ["/v1/health", "/v1/version"]
FORBIDDEN_IMPORTS = {"sqlalchemy", "alembic", "psycopg", "psycopg2", "asyncpg", "argon2", "jwt", "jose"}

failures: list[str] = []


def report(ok: bool, name: str, detail: str = "") -> bool:
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" - {detail}" if detail else ""))
    if not ok:
        failures.append(name)
    return ok


def check_sources() -> None:
    bad, forbidden = [], []
    for path in sorted(list((SERVER / "app").rglob("*.py")) + list((SERVER / "tests").rglob("*.py"))):
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except SyntaxError as exc:
            bad.append(f"{path.relative_to(ROOT)}:{exc.lineno}")
            continue
        for node in ast.walk(tree):
            names = [a.name for a in node.names] if isinstance(node, ast.Import) else \
                    [node.module or ""] if isinstance(node, ast.ImportFrom) else []
            if any(n.split(".")[0] in FORBIDDEN_IMPORTS for n in names):
                forbidden.append(str(path.relative_to(ROOT)))
    report(not bad, "python syntax", ", ".join(bad))
    report(not forbidden, "no out-of-scope imports (db/auth)", ", ".join(sorted(set(forbidden))))


def check_contract() -> None:
    text = (ROOT / "api" / "openapi.yaml").read_text(encoding="utf-8")
    missing = [p for p in CONTRACT_PATHS if not re.search(rf"^  {re.escape(p)}:\s*$", text, re.M)]
    report(not missing, "openapi declares the implemented paths", ", ".join(missing))
    routes = (SERVER / "app" / "routes" / "meta.py").read_text(encoding="utf-8")
    ops = re.findall(r'operation_id="(\w+)"', routes)
    report(all(f"operationId: {o}" in text for o in ops) and bool(ops), "operation ids match the contract", ", ".join(ops))


def check_dependencies() -> bool:
    missing = [m for m in REQUIRED if importlib.util.find_spec(m) is None]
    if missing:
        print(f"[MISSING] packages not installed: {', '.join(missing)} (this script never installs; see server/README.md)")
    else:
        print("[PASS] required packages importable")
    return not missing


def run_tests() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "PYTHONPYCACHEPREFIX": str(OUT / "pycache")}
    for key in [k for k in env if k.startswith("TASKNEST_")]:
        del env[key]  # tests must not depend on the caller's environment
    proc = subprocess.run([sys.executable, "-m", "pytest", "-q"], cwd=SERVER, env=env)
    report(proc.returncode == 0, "pytest", f"exit code {proc.returncode}")


def main() -> int:
    check_sources()
    check_contract()
    if not check_dependencies():
        print("RESULT: INCOMPLETE (static checks " + ("passed" if not failures else "FAILED") + ", tests not run)")
        return 1 if failures else 2
    run_tests()
    print("RESULT: " + ("FAIL: " + ", ".join(failures) if failures else "PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
