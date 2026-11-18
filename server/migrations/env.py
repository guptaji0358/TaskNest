"""Alembic environment: async engine, URL chosen by app.db.url.migration_url only.

dev: TASKNEST_MIGRATION_DATABASE_URL, else TASKNEST_DATABASE_URL.
test: TASKNEST_MIGRATION_DATABASE_URL, else TASKNEST_TEST_DATABASE_URL (never the dev URL).
prod: TASKNEST_MIGRATION_DATABASE_URL only, explicit and direct (the operator supplies a direct
endpoint; this cannot be verified). Every URL is validated for its environment, and production
uses certificate- and hostname-verified TLS. Failures print fixed text only.
"""
from __future__ import annotations

import asyncio
import os

from alembic import context
from sqlalchemy.ext.asyncio import create_async_engine

from app.db.errors import translate
from app.db.tables import metadata
from app.db.url import DatabaseUrlError, connect_args, migration_url

config = context.config
if config.config_file_name is not None:
    from logging.config import fileConfig
    fileConfig(config.config_file_name)


def _environment() -> str:
    value = os.environ.get("TASKNEST_ENV", "dev").strip().lower()
    if value not in ("dev", "test", "prod"):
        raise SystemExit("TASKNEST_ENV must be one of dev, test, prod")
    return value


def _url(environment: str):
    message = None
    url = None
    try:
        url = migration_url(os.environ, environment)
    except DatabaseUrlError as exc:
        message = str(exc)
    if message is not None:
        raise SystemExit(message)  # raised outside the except block: no __context__
    return url


def _run(connection) -> None:
    context.configure(connection=connection, target_metadata=metadata, compare_type=True)
    with context.begin_transaction():
        context.run_migrations()


async def _run_async(environment: str) -> None:
    engine = create_async_engine(_url(environment), hide_parameters=True,
                                 connect_args=connect_args(environment))
    message = None
    try:
        async with engine.connect() as conn:
            await conn.run_sync(_run)
    except Exception as exc:  # noqa: BLE001 - reduced to a fixed message below
        translated = translate(exc, "migrate", driver_call=True)
        message = (str(translated) if translated is not None
                   else f"migration failed ({type(exc).__name__})")
    finally:
        await engine.dispose()
    if message is not None:
        raise SystemExit(message)


if context.is_offline_mode():
    raise SystemExit("offline migrations are not supported")
asyncio.run(_run_async(_environment()))
