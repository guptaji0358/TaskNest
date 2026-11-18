"""Async engine and transaction helpers. No query logic lives here.

Error policy (details in errors.py): database failures leave this module as sanitized
``DatabaseError`` values raised outside any ``except`` block (connect, begin, commit and run());
application exceptions, and database errors from a bare conn.execute(), pass through as the same object. A cleanup failure (rollback, close, invalidate) never replaces
the primary exception: it is logged with fixed fields and the connection is invalidated.
"""
from __future__ import annotations

from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

from sqlalchemy.engine import URL
from sqlalchemy.ext.asyncio import AsyncConnection, AsyncEngine, create_async_engine

from app.db.errors import note, translate
from app.db.url import DatabaseUrlError, connect_args


def make_engine(url: URL | None, environment: str = "dev", *, pool_size: int = 5) -> AsyncEngine:
    """Build the application engine from a validated URL object (see app.db.url).

    A missing URL is refused before any engine exists; in production that is a hard error.
    hide_parameters only hides bound SQL values in error text; it does not protect the URL.
    """
    if url is None:
        raise DatabaseUrlError("TASKNEST_DATABASE_URL is required in production" if environment == "prod"
                               else "the database URL is not configured")
    return create_async_engine(url, pool_size=pool_size, pool_pre_ping=True, hide_parameters=True,
                               connect_args=connect_args(environment))


async def _invalidate(conn: AsyncConnection) -> None:
    try:
        await conn.invalidate()
    except Exception as exc:  # noqa: BLE001 - must never replace the primary exception
        note("invalidate", exc)


async def _close(conn: AsyncConnection) -> None:
    try:
        await conn.close()
    except Exception as exc:  # noqa: BLE001 - logged, never raised; the work is already decided
        note("close", exc)
        await _invalidate(conn)


@asynccontextmanager
async def connection(engine: AsyncEngine) -> AsyncIterator[AsyncConnection]:
    """A connection for reads; closed on exit."""
    failure = None
    try:
        conn = await engine.connect()
    except Exception as exc:
        failure = translate(exc, "connect")
        if failure is None:
            raise
    if failure is not None:
        raise failure  # outside the except block: no __context__, no __cause__
    try:
        yield conn
    except BaseException:
        await _close(conn)
        raise  # the same object: database errors from run() are already sanitized
    else:
        await _close(conn)


async def run(conn: AsyncConnection, statement, parameters=None):
    """Execute one statement; the only place a query-phase failure is translated.

    While this call runs only our driver call is executing, so a raw OSError or unmapped asyncpg
    error here is driver-originated and is translated. An application OSError raised in a block
    never passes through run(), so it is never misclassified. Translating here, outside any
    ``with`` cleanup, also keeps ``__context__`` empty: an asynccontextmanager would raise while
    the body's exception is still being handled and Python would attach it as ``__context__``.
    Repository code must use run() rather than conn.execute().
    """
    failure = None
    try:
        return await conn.execute(statement, parameters)
    except Exception as exc:
        failure = translate(exc, "query", driver_call=True)
        if failure is None:
            raise
    raise failure


@asynccontextmanager
async def transaction(engine: AsyncEngine) -> AsyncIterator[AsyncConnection]:
    """One connection, one transaction: commits on success, rolls back on any exception."""
    async with connection(engine) as conn:
        failure = None
        try:
            trans = await conn.begin()
        except Exception as exc:
            failure = translate(exc, "begin")
            if failure is None:
                raise
        if failure is not None:
            raise failure
        try:
            yield conn
        except BaseException:
            try:
                await trans.rollback()
            except Exception as exc:  # noqa: BLE001 - the original exception stays primary
                note("rollback", exc)
                await _invalidate(conn)
            raise  # the original exception, unchanged
        else:
            try:
                await trans.commit()
            except Exception as exc:
                await _invalidate(conn)
                failure = translate(exc, "commit")
                if failure is None:
                    raise
            if failure is not None:
                raise failure
