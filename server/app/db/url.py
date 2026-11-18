"""Database URL policy (M2.1b).

Three variables, three roles:

* TASKNEST_DATABASE_URL            application connection (dev and prod)
* TASKNEST_MIGRATION_DATABASE_URL  Alembic connection; explicit and mandatory in prod
* TASKNEST_TEST_DATABASE_URL       test application and test migration connection

Every error is a fixed message naming only the variable and the rule. No URL
component (user, password, host, database, query) ever appears in a message, and
errors are raised outside any ``except`` block so no raw exception is attached as
``__context__``.

``TASKNEST_DATABASE_POOLED=false`` (the default) is an operator assertion that the
application URL is a direct, non-pooled endpoint. The application cannot verify
that. ``true`` is rejected: pooled endpoints are not supported in M2.1b.
"""
from __future__ import annotations

import ssl
from collections.abc import Mapping

from sqlalchemy.engine import URL, make_url

APP = "TASKNEST_DATABASE_URL"
MIGRATION = "TASKNEST_MIGRATION_DATABASE_URL"
TEST = "TASKNEST_TEST_DATABASE_URL"
DRIVER = "postgresql+asyncpg"
LOCAL_HOSTS = frozenset({"localhost", "127.0.0.1", "::1"})
DATABASE_NAMES = {"dev": "tasknest_dev", "test": "tasknest_test"}


class DatabaseUrlError(ValueError):
    """Invalid database URL or settings. The message is fixed and never carries URL text."""


def parse(text: str, name: str) -> URL:
    """Parse into a SQLAlchemy URL object. Any query parameter is rejected."""
    url = None
    try:
        url = make_url(text)
    except Exception:  # noqa: BLE001 - the parser's message may quote the URL, so it is dropped
        pass
    if url is None:
        raise DatabaseUrlError(f"{name} is malformed")
    if url.drivername != DRIVER:
        raise DatabaseUrlError(f"{name} must use {DRIVER}")
    if not url.host:
        raise DatabaseUrlError(f"{name} must name a host")
    if url.query or "?" in text:  # a blank-valued parameter ("?ssl=") is dropped by the parser
        keys = {str(k).strip().lower() for k in url.query}
        if keys & {"ssl", "sslmode"}:
            raise DatabaseUrlError(f"{name}: ssl and sslmode parameters are not allowed")
        raise DatabaseUrlError(f"{name}: URL query parameters are not allowed")
    return url


def validate(url: URL, environment: str, name: str) -> URL:
    """Environment rules. Application and migration URLs share them."""
    if environment in DATABASE_NAMES:
        if url.host.lower() not in LOCAL_HOSTS:
            raise DatabaseUrlError(f"{name} must use a local host")
        if url.database != DATABASE_NAMES[environment]:
            raise DatabaseUrlError(f"{name} must name database {DATABASE_NAMES[environment]}")
    elif not url.database:
        raise DatabaseUrlError(f"{name} must name a database")
    return url  # prod: any host; verified TLS is applied through connect_args


def _text(environ: Mapping[str, str], name: str) -> str | None:
    return environ.get(name, "").strip() or None


def application_url(environ: Mapping[str, str], environment: str) -> URL | None:
    """Test reads only the test URL; dev and prod read only the application URL.

    Absence is allowed here (``None``); make_engine rejects it in production.
    """
    name = TEST if environment == "test" else APP
    text = _text(environ, name)
    return None if text is None else validate(parse(text, name), environment, name)


def migration_url(environ: Mapping[str, str], environment: str) -> URL:
    """dev: migration URL, else application URL. test: migration URL, else test URL (never the
    dev URL). prod: the explicit migration URL only, never the application URL."""
    names = {"prod": (MIGRATION,), "test": (MIGRATION, TEST), "dev": (MIGRATION, APP)}[environment]
    for name in names:
        text = _text(environ, name)
        if text is not None:
            return validate(parse(text, name), environment, name)
    raise DatabaseUrlError(f"{' or '.join(names)} is not set")


def parse_pooled(raw: str | None) -> bool:
    """TASKNEST_DATABASE_POOLED: ``false`` (default) asserts a direct endpoint; ``true`` is unsupported."""
    value = (raw or "").strip().lower() or "false"
    if value == "false":
        return False
    if value == "true":
        raise DatabaseUrlError("TASKNEST_DATABASE_POOLED: pooled endpoints are not supported yet")
    raise DatabaseUrlError("TASKNEST_DATABASE_POOLED must be true or false")


def connect_args(environment: str) -> dict:
    """Production: certificate and hostname verification (CERT_REQUIRED, check_hostname, system CAs)."""
    return {"ssl": ssl.create_default_context()} if environment == "prod" else {}
