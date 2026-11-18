"""Exception translation, phases, cleanup precedence, chaining and redaction. Fakes only: no database."""
import asyncio
import logging
import re
import ssl
import traceback

import asyncpg
import pytest
import sqlalchemy.exc as sa_exc

from app.db.errors import DatabaseError, DatabaseUnavailable, categorize, translate
from app.db.session import connection, run, transaction

CANARIES = ("zq_user_7f3a", "P4ss-w0rd-XK9QJ7", "db-leak-canary.invalid")
LEAK = "user=zq_user_7f3a password=P4ss-w0rd-XK9QJ7 host=db-leak-canary.invalid"
LOG_LINE = re.compile(r"^database (close|rollback|invalidate) failed \(\w+\) sqlstate=\w+$")


class Orig(Exception):
    def __init__(self, sqlstate=None):
        super().__init__(LEAK)
        self.sqlstate = sqlstate


def dbapi(sqlstate=None, cls=sa_exc.OperationalError):
    return cls("select 1", {}, Orig(sqlstate))


class AppError(Exception):
    pass


class Trans:
    def __init__(self, conn):
        self.conn = conn

    async def commit(self):
        await self.conn.step("commit")

    async def rollback(self):
        await self.conn.step("rollback")


class Conn:
    def __init__(self, script=None):
        self.script, self.calls = script or {}, []

    async def step(self, name):
        self.calls.append(name)
        if name in self.script:
            raise self.script[name]

    async def begin(self):
        await self.step("begin")
        return Trans(self)

    async def close(self):
        await self.step("close")

    async def invalidate(self):
        await self.step("invalidate")

    async def execute(self, statement, parameters=None):
        await self.step("execute")
        return "result"


class Engine:
    def __init__(self, conn=None, connect_error=None):
        self.conn, self.connect_error = conn or Conn(), connect_error

    async def connect(self):
        if self.connect_error:
            raise self.connect_error
        return self.conn


def clean(exc, raw=()):
    """Assert a public exception exposes nothing of the raw one: text, context, cause, traceback, frames."""
    assert exc.__context__ is None and exc.__cause__ is None
    text = str(exc) + repr(exc) + "".join(traceback.format_exception(exc))
    assert not any(c in text for c in CANARIES)
    tb = exc.__traceback__
    while tb is not None:
        if re.search(r"app[\/]db[\/]", tb.tb_frame.f_code.co_filename):  # our frames, not the test's own
            assert not any(v is r for v in tb.tb_frame.f_locals.values() for r in raw)
        tb = tb.tb_next


def go(coro):
    return asyncio.run(coro)


# --- SQLSTATE classification ------------------------------------------------------------------------

@pytest.mark.parametrize("code,category", [
    ("53100", "resource"), ("53200", "resource"), ("53300", "resource"), ("53000", "resource"),
    ("57014", "canceled"), ("57P01", "connection"), ("57P02", "connection"), ("57P03", "connection"),
    ("28P01", "authentication"), ("28000", "authentication"),
    ("08006", "connection"), ("08999", "connection"), ("23505", "integrity"), ("23503", "integrity"),
    ("40001", "retryable"), ("40P01", "retryable"), ("40003", "query"),
    ("53999", "query"), ("57999", "query"), ("99999", "query"), ("57P04", "query"),
])
def test_sqlstate_categories(code, category):
    assert categorize(dbapi(code), code) == category
    assert translate(dbapi(code), "query").category == category


def test_unavailable_class_only_for_connection_authentication_resource():
    assert isinstance(translate(dbapi("28P01"), "query"), DatabaseUnavailable)
    assert not isinstance(translate(dbapi("23505"), "query"), DatabaseUnavailable)


def test_no_sqlstate_falls_back_to_type_signals():
    assert translate(dbapi(None), "query").category == "query"
    assert translate(sa_exc.TimeoutError("pool"), "query").category == "resource"
    assert translate(dbapi(None, sa_exc.InterfaceError), "query").category == "connection"
    assert translate(sa_exc.DisconnectionError(LEAK), "query").category == "connection"


@pytest.mark.parametrize("exc", [sa_exc.InvalidRequestError(LEAK), sa_exc.ArgumentError(LEAK),
                                 sa_exc.PendingRollbackError(LEAK), sa_exc.StatementError(LEAK, "s", {}, ValueError()),
                                 ValueError(LEAK), AppError(LEAK), OSError(LEAK)])
def test_application_side_exceptions_are_never_translated_in_the_query_phase(exc):
    assert translate(exc, "query") is None


def test_raw_io_is_translated_only_where_our_driver_call_runs():
    assert translate(OSError(LEAK), "connect").category == "connection"
    assert translate(OSError(LEAK), "query", driver_call=True).category == "connection"
    assert translate(ssl.SSLError(LEAK), "begin") is not None
    assert translate(asyncio.TimeoutError(LEAK), "commit") is not None


# --- the six phases ------------------------------------------------------------------------------------

@pytest.mark.parametrize("raw", [OSError(LEAK), ssl.SSLError(LEAK), TimeoutError(LEAK), asyncpg.InterfaceError(LEAK),
                                 dbapi("08006")])
def test_connect_phase(raw):
    async def body():
        async with connection(Engine(connect_error=raw)):
            pass
    with pytest.raises(DatabaseError) as exc:
        go(body())
    assert exc.value.phase == "connect" and exc.value.category == "connection"
    clean(exc.value, [raw])


def test_begin_phase_is_begin_not_query():
    raw = dbapi("08006")
    conn = Conn({"begin": raw})
    async def body():
        async with transaction(Engine(conn)):
            pass
    with pytest.raises(DatabaseError) as exc:
        go(body())
    assert exc.value.phase == "begin" and conn.calls == ["begin", "close"]
    clean(exc.value, [raw])


@pytest.mark.parametrize("code,category", [("23505", "integrity"), ("40001", "retryable"), (None, "query")])
def test_query_phase_through_run(code, category):
    raw = dbapi(code, sa_exc.IntegrityError if code == "23505" else sa_exc.OperationalError)
    async def body():
        async with transaction(Engine(Conn({"execute": raw}))) as c:
            await run(c, "select 1")
    with pytest.raises(DatabaseError) as exc:
        go(body())
    assert (exc.value.phase, exc.value.category, exc.value.sqlstate) == ("query", category, code)
    clean(exc.value, [raw])


def test_run_translates_a_raw_driver_oserror_but_not_application_code():
    raw = OSError(LEAK)
    async def body():
        await run(Conn({"execute": raw}), "select 1")
    with pytest.raises(DatabaseError) as exc:
        go(body())
    assert exc.value.category == "connection"
    clean(exc.value, [raw])


def test_commit_phase():
    raw = dbapi("40001")
    conn = Conn({"commit": raw})
    async def body():
        async with transaction(Engine(conn)):
            pass
    with pytest.raises(DatabaseError) as exc:
        go(body())
    assert exc.value.phase == "commit" and exc.value.category == "retryable"
    assert conn.calls == ["begin", "commit", "invalidate", "close"]
    clean(exc.value, [raw])


def test_rollback_phase_is_logged_and_the_original_stays_primary(caplog):
    original = AppError("application failure")
    conn = Conn({"rollback": dbapi("08006")})
    async def body():
        async with transaction(Engine(conn)):
            raise original
    with caplog.at_level(logging.DEBUG, logger="tasknest.db"), pytest.raises(AppError) as exc:
        go(body())
    assert exc.value is original
    assert conn.calls == ["begin", "rollback", "invalidate", "close"]
    assert [r.getMessage() for r in caplog.records] == ["database rollback failed (connection) sqlstate=08006"]


def test_close_phase_is_logged_not_raised_after_success(caplog):
    conn = Conn({"close": OSError(LEAK)})
    async def body():
        async with transaction(Engine(conn)) as c:
            return await run(c, "select 1")
    with caplog.at_level(logging.DEBUG, logger="tasknest.db"):
        assert go(body()) == "result"
    assert conn.calls == ["begin", "execute", "commit", "close", "invalidate"]
    messages = [r.getMessage() for r in caplog.records]
    assert messages == ["database close failed (connection) sqlstate=none"]


# --- cleanup precedence -------------------------------------------------------------------------------

def run_case(script, raiser=None, db_error=None):
    conn = Conn(script)
    async def body():
        async with transaction(Engine(conn)) as c:
            if db_error is not None:
                await run(c, "select 1")
            if raiser is not None:
                raise raiser
    return conn, body


def test_application_error_rollback_ok_close_fails(caplog):
    original = AppError("boom")
    conn, body = run_case({"close": OSError(LEAK)}, raiser=original)
    with caplog.at_level(logging.DEBUG, logger="tasknest.db"), pytest.raises(AppError) as exc:
        go(body())
    assert exc.value is original and "invalidate" in conn.calls
    assert all(LOG_LINE.match(r.getMessage()) for r in caplog.records) and len(caplog.records) == 1


def test_application_error_rollback_fails_close_fails(caplog):
    original = AppError("boom")
    conn, body = run_case({"rollback": OSError(LEAK), "close": OSError(LEAK)}, raiser=original)
    with caplog.at_level(logging.DEBUG, logger="tasknest.db"), pytest.raises(AppError) as exc:
        go(body())
    assert exc.value is original
    phases = [r.getMessage().split()[1] for r in caplog.records]
    assert phases == ["rollback", "close"] and all(LOG_LINE.match(r.getMessage()) for r in caplog.records)


def test_database_query_error_close_fails(caplog):
    conn, body = run_case({"execute": dbapi("23505"), "close": OSError(LEAK)}, db_error=True)
    with caplog.at_level(logging.DEBUG, logger="tasknest.db"), pytest.raises(DatabaseError) as exc:
        go(body())
    assert exc.value.phase == "query" and exc.value.category == "integrity"
    assert [r.getMessage().split()[1] for r in caplog.records] == ["close"]


def test_commit_failure_close_fails(caplog):
    conn, body = run_case({"commit": dbapi("40001"), "close": OSError(LEAK)})
    with caplog.at_level(logging.DEBUG, logger="tasknest.db"), pytest.raises(DatabaseError) as exc:
        go(body())
    assert exc.value.phase == "commit"
    assert [r.getMessage().split()[1] for r in caplog.records] == ["close"]


def test_invalidate_failure_never_replaces_the_primary_exception(caplog):
    original = AppError("boom")
    conn, body = run_case({"rollback": OSError(LEAK), "invalidate": OSError(LEAK)}, raiser=original)
    with caplog.at_level(logging.DEBUG, logger="tasknest.db"), pytest.raises(AppError) as exc:
        go(body())
    assert exc.value is original
    assert all(LOG_LINE.match(r.getMessage()) for r in caplog.records)


def test_cancellation_survives_a_failing_rollback():
    conn, body = run_case({"rollback": OSError(LEAK)}, raiser=asyncio.CancelledError())
    with pytest.raises(asyncio.CancelledError):
        go(body())


def test_log_records_carry_no_secret_and_no_exc_info(caplog):
    conn, body = run_case({"rollback": OSError(LEAK), "close": OSError(LEAK)}, raiser=AppError("x"))
    with caplog.at_level(logging.DEBUG), pytest.raises(AppError):
        go(body())
    for record in caplog.records:
        assert record.exc_info is None and not any(c in caplog.text for c in CANARIES)


# --- application exceptions are untouched ----------------------------------------------------------------

@pytest.mark.parametrize("original", [OSError(LEAK), ValueError("v"), AppError("a"), sa_exc.InvalidRequestError("r")])
def test_application_exceptions_pass_through_as_the_same_object_and_roll_back(original):
    conn, body = run_case({}, raiser=original)
    with pytest.raises(type(original)) as exc:
        go(body())
    assert exc.value is original and conn.calls == ["begin", "rollback", "close"]


def test_application_oserror_inside_a_transaction_is_not_a_database_error():
    original = OSError(2, "no such file")
    conn, body = run_case({}, raiser=original)
    with pytest.raises(OSError) as exc:
        go(body())
    assert exc.value is original and not isinstance(exc.value, DatabaseError)


# --- characterization of the real SQLAlchemy asyncpg adapter (records behaviour; see report) -------------

def test_characterize_adapter_with_unmapped_raw_errors():
    """What does the installed adapter do with errors it cannot map? The result decides whether raw
    driver errors can escape. Compare to the pinned versions once the isolated environment exists."""
    from sqlalchemy.dialects.postgresql import asyncpg as dialect
    adapter = dialect.PGDialect_asyncpg.import_dbapi()  # the adapter instance holding the error table
    mapped = []
    for raw in (OSError(LEAK), ConnectionResetError(LEAK), asyncpg.InterfaceError(LEAK),
                asyncpg.exceptions.ConnectionDoesNotExistError(LEAK)):
        table = adapter._asyncpg_error_translate
        mapped.append((type(raw).__name__, any(base in table for base in type(raw).__mro__)))
    # Observed, not asserted as a design fact: record it so the report can state it.
    print("MAPPED_BY_ADAPTER", mapped)
    assert mapped[0] == ("OSError", False)  # a raw OSError has no mapping, so the adapter cannot wrap it
    assert mapped[2][1] is True and mapped[3][1] is True  # asyncpg's own errors are mapped
