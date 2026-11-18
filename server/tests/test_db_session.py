"""Engine and transaction helper (no database needed except the round-trip test)."""
import asyncio
import os

import pytest
import sqlalchemy as sa

from app.config import ConfigError, load_settings
from app.db.session import make_engine, transaction
from app.db.url import application_url


def test_database_url_is_optional_and_validated():
    assert load_settings({}).database_url is None
    url = "postgresql+asyncpg://u:p@127.0.0.1:5433/tasknest_dev"
    assert load_settings({"TASKNEST_DATABASE_URL": url}).database_url.database == "tasknest_dev"
    with pytest.raises(ConfigError) as exc:
        load_settings({"TASKNEST_DATABASE_URL": "mysql://u:secret@h/db"})
    assert "secret" not in str(exc.value)


def test_make_engine_requires_a_url():
    with pytest.raises(ValueError):
        make_engine(None)


@pytest.mark.skipif(not os.environ.get("TASKNEST_TEST_DATABASE_URL"), reason="TASKNEST_TEST_DATABASE_URL not set")
def test_transaction_commits_and_rolls_back():
    async def go():
        engine = make_engine(application_url({"TASKNEST_TEST_DATABASE_URL": os.environ["TASKNEST_TEST_DATABASE_URL"]}, "test"), "test")
        try:
            async with transaction(engine) as c:
                await c.execute(sa.text("create temp table t(x int)"))
            with pytest.raises(RuntimeError):
                async with transaction(engine) as c:
                    assert (await c.execute(sa.text("select 1"))).scalar_one() == 1
                    raise RuntimeError
        finally:
            await engine.dispose()
    asyncio.run(go())
