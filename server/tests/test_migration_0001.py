"""Migration 0001 against a dedicated PostgreSQL test database (TASKNEST_TEST_DATABASE_URL)."""
import asyncio
import datetime as dt
import os
import subprocess
import sys
import uuid
from pathlib import Path

import pytest
import sqlalchemy as sa
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import create_async_engine

URL = os.environ.get("TASKNEST_TEST_DATABASE_URL", "")
pytestmark = pytest.mark.skipif(not URL, reason="TASKNEST_TEST_DATABASE_URL not set")
SERVER = Path(__file__).resolve().parents[1]
EXPECTED = {"users", "devices", "refresh_families", "refresh_tokens", "access_tokens", "alembic_version"}
NOW = dt.datetime(2026, 1, 1, tzinfo=dt.timezone.utc)


def alembic(*args: str) -> None:
    env = {k: v for k, v in os.environ.items()
           if k not in ("TASKNEST_DATABASE_URL", "TASKNEST_MIGRATION_DATABASE_URL", "PYTHONPATH")}
    env.update(TASKNEST_ENV="test", TASKNEST_TEST_DATABASE_URL=URL)  # exercises the test fallback
    subprocess.run([sys.executable, "-m", "alembic", *args], cwd=SERVER, env=env, check=True,
                   capture_output=True, text=True)


def run(coro_fn):
    async def go():
        engine = create_async_engine(URL)
        try:
            return await coro_fn(engine)
        finally:
            await engine.dispose()
    return asyncio.run(go())


async def _tables(engine):
    async with engine.connect() as c:
        return set(await c.run_sync(lambda s: sa.inspect(s).get_table_names()))


@pytest.fixture(autouse=True)
def fresh():
    alembic("downgrade", "base")
    alembic("upgrade", "head")
    yield
    alembic("downgrade", "base")


def test_creates_exactly_the_account_tables():
    assert run(_tables) == EXPECTED


def test_downgrade_removes_them():
    alembic("downgrade", "base")
    assert run(_tables) == {"alembic_version"}


async def _seed(c, login="a"):
    uid, did, fid = (uuid.uuid4() for _ in range(3))
    await c.execute(sa.text("insert into users(id, login, password_hash) values (:i, :l, 'h')"), {"i": uid, "l": login})
    await c.execute(sa.text("insert into devices values (:d, :u, 'n', 'p', 'v', :t, :t)"), {"d": did, "u": uid, "t": NOW})
    await c.execute(sa.text("insert into refresh_families(id, account_id, device_id, created_at, absolute_expires_at, "
                            "sliding_expires_at) values (:f, :u, :d, :t, :t, :t)"), {"f": fid, "u": uid, "d": did, "t": NOW})
    return uid, did, fid


def test_access_token_with_foreign_account_is_rejected():
    async def go(engine):
        async with engine.begin() as c:
            _, _, fid = await _seed(c, "a")
            other, _, _ = await _seed(c, "b")
        with pytest.raises(IntegrityError):
            async with engine.begin() as c:
                await c.execute(sa.text("insert into access_tokens values (:i, :h, :f, :a, :t, :t)"),
                                {"i": uuid.uuid4(), "h": b"x" * 32, "f": fid, "a": other, "t": NOW})
    run(go)


def test_one_family_per_device_and_unique_rotation_operation():
    async def go(engine):
        async with engine.begin() as c:
            uid, did, fid = await _seed(c)
        with pytest.raises(IntegrityError):
            async with engine.begin() as c:
                await c.execute(sa.text("insert into refresh_families(id, account_id, device_id, created_at, "
                                        "absolute_expires_at, sliding_expires_at) values (:f, :u, :d, :t, :t, :t)"),
                                {"f": uuid.uuid4(), "u": uid, "d": did, "t": NOW})
        op = uuid.uuid4()
        async with engine.begin() as c:
            await c.execute(sa.text("insert into refresh_tokens values (:i, :f, 1, :h, :t, :t, :o)"),
                            {"i": uuid.uuid4(), "f": fid, "h": b"1" * 32, "t": NOW, "o": op})
        with pytest.raises(IntegrityError):
            async with engine.begin() as c:
                await c.execute(sa.text("insert into refresh_tokens values (:i, :f, 2, :h, :t, :t, :o)"),
                                {"i": uuid.uuid4(), "f": fid, "h": b"2" * 32, "t": NOW, "o": op})
    run(go)


def test_partial_rotation_index_exists():
    async def go(engine):
        async with engine.connect() as c:
            return (await c.execute(sa.text("select indexdef from pg_indexes where "
                                            "indexname = 'ix_refresh_tokens_family_rotated'"))).scalar_one()
    assert "WHERE (rotated_at IS NOT NULL)" in run(go)
