"""Sanitized database exceptions (M2.1b).

Public exceptions carry only a fixed message, a category, a phase and a SQLSTATE code.
They never hold a reference to the raw driver exception, and callers raise them outside
any ``except`` block so ``__context__`` and ``__cause__`` stay ``None``. ``from None``
alone would only hide the context when printed; it would not clear it.

Which exceptions are database failures is decided by type:

* ``DATABASE`` types are always database failures. SQLAlchemy's ``DBAPIError`` is a
  server or driver failure; its siblings that are programming/usage errors
  (``InvalidRequestError``, ``ArgumentError``, a non-DBAPI ``StatementError``) are
  application-side and are never translated.
* ``DRIVER_ONLY`` types (OSError, timeouts, ssl) are translated only in phases where
  nothing but our own driver call is running (connect, begin, commit, rollback, close,
  or an explicit ``driver_call``). An OSError raised by application code is never touched.
"""
from __future__ import annotations

import asyncio
import logging
import ssl

import asyncpg
import sqlalchemy.exc as sa_exc

log = logging.getLogger("tasknest.db")

CATEGORIES = ("connection", "authentication", "resource", "canceled", "integrity", "retryable", "query")

_AUTH = frozenset({"28000", "28P01"})
_RESOURCE = frozenset({"53000", "53100", "53200", "53300", "53400"})
_CONNECTION = frozenset({"57P01", "57P02", "57P03"})  # plus the whole of class 08
_CANCELED = frozenset({"57014"})
_RETRYABLE = frozenset({"40001", "40P01"})  # 40003 (outcome unknown) is deliberately not retryable

DATABASE = (sa_exc.DBAPIError, sa_exc.DisconnectionError, sa_exc.TimeoutError,
            asyncpg.PostgresError, asyncpg.InterfaceError)
DRIVER_ONLY = (OSError, asyncio.TimeoutError, ssl.SSLError)
DRIVER_CALL_PHASES = frozenset({"connect", "begin", "commit", "rollback", "close", "invalidate"})


class DatabaseError(Exception):
    """Public database failure. Message format: ``database <phase> failed (<category>)``."""

    def __init__(self, phase: str, category: str, sqlstate: str | None = None) -> None:
        super().__init__(f"database {phase} failed ({category})")
        self.phase = phase
        self.category = category
        self.sqlstate = sqlstate


class DatabaseUnavailable(DatabaseError):
    """connection, authentication or resource failure."""


def _sqlstate(exc: BaseException) -> str | None:
    code = getattr(getattr(exc, "orig", None), "sqlstate", None) or getattr(exc, "sqlstate", None)
    return code if isinstance(code, str) and len(code) == 5 else None


def categorize(exc: BaseException, code: str | None) -> str:
    if code is not None:
        if code in _AUTH:
            return "authentication"
        if code in _RESOURCE:
            return "resource"
        if code in _CANCELED:
            return "canceled"
        if code[:2] == "08" or code in _CONNECTION:
            return "connection"
        if code[:2] == "23":
            return "integrity"
        if code in _RETRYABLE:
            return "retryable"
        return "query"
    if isinstance(exc, sa_exc.TimeoutError):
        return "resource"
    if (getattr(exc, "connection_invalidated", False)
            or isinstance(exc, (sa_exc.DisconnectionError, sa_exc.InterfaceError, asyncpg.InterfaceError))
            or isinstance(exc, DRIVER_ONLY)):
        return "connection"
    return "query"


def translate(exc: BaseException, phase: str, *, driver_call: bool = False) -> DatabaseError | None:
    """Return a sanitized error for a database failure, or ``None`` for anything else.

    The caller must raise the result outside its ``except`` block.
    """
    if isinstance(exc, DATABASE) or (isinstance(exc, DRIVER_ONLY) and (driver_call or phase in DRIVER_CALL_PHASES)):
        code = _sqlstate(exc)
        category = categorize(exc, code)
        cls = DatabaseUnavailable if category in ("connection", "authentication", "resource") else DatabaseError
        return cls(phase, category, code)
    return None


def note(phase: str, exc: BaseException) -> None:
    """Log a cleanup failure with fixed fields only. Never raises, never uses exc_info or str(exc)."""
    translated = translate(exc, phase, driver_call=True)
    category = translated.category if translated else "unclassified"
    sqlstate = (translated.sqlstate if translated else None) or "none"
    log.warning("database %s failed (%s) sqlstate=%s", phase, category, sqlstate)
