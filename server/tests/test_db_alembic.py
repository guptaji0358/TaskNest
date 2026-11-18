"""env.py failure handling and URL selection without touching any database (no connection succeeds)."""
import os
import subprocess
import sys
from pathlib import Path

import pytest

SERVER = Path(__file__).resolve().parents[1]
USER, PASSWORD, HOST = "zq_user_7f3a", "P4ss-w0rd-XK9QJ7", "db-leak-canary.invalid"
SECRETS = ("TASKNEST_DATABASE_URL", "TASKNEST_MIGRATION_DATABASE_URL", "TASKNEST_TEST_DATABASE_URL", "TASKNEST_ENV")


def url(host="127.0.0.1:1", db="tasknest_test"):
    return f"postgresql+asyncpg://{USER}:{PASSWORD}@{host}/{db}"


def alembic(args=("upgrade", "head"), **env):
    clean = {k: v for k, v in os.environ.items() if k not in SECRETS}
    done = subprocess.run([sys.executable, "-m", "alembic", *args], cwd=SERVER, env={**clean, **env},
                          capture_output=True, text=True, timeout=60)
    output = done.stdout + done.stderr
    assert not any(c in output for c in (USER, PASSWORD, HOST)), "a secret reached the output"
    return done.returncode, output


def test_test_environment_never_falls_back_to_the_dev_url():
    code, out = alembic(TASKNEST_ENV="test", TASKNEST_DATABASE_URL=url(db="tasknest_dev"))
    assert code != 0 and "TASKNEST_MIGRATION_DATABASE_URL or TASKNEST_TEST_DATABASE_URL is not set" in out


def test_prod_requires_an_explicit_migration_url():
    code, out = alembic(TASKNEST_ENV="prod", TASKNEST_DATABASE_URL=url(HOST, "app"))
    assert code != 0 and "TASKNEST_MIGRATION_DATABASE_URL is not set" in out


def test_explicit_test_migration_url_cannot_bypass_test_restrictions():
    code, out = alembic(TASKNEST_ENV="test", TASKNEST_MIGRATION_DATABASE_URL=url(HOST),
                        TASKNEST_TEST_DATABASE_URL=url())
    assert code != 0 and "must use a local host" in out


def test_connection_failure_prints_only_fixed_text():
    code, out = alembic(TASKNEST_ENV="test", TASKNEST_TEST_DATABASE_URL=url())  # nothing listens on port 1
    assert code != 0 and "database migrate failed (connection)" in out


def test_offline_mode_is_not_supported():
    code, out = alembic(("upgrade", "head", "--sql"), TASKNEST_ENV="test", TASKNEST_TEST_DATABASE_URL=url())
    assert code != 0 and "offline migrations are not supported" in out
