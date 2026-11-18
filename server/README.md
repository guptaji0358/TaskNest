# TaskNest server (M2.1 skeleton)

Optional cloud backend. M2.1 implements only `GET /v1/health` and `GET /v1/version`, configuration, the
JSON error envelope, request IDs and structured logging. No accounts, authentication, database or sync.
The wire contract is `../api/openapi.yaml`; the plan is `../CLOUD.md` section 16.

## Layout

- `app/main.py` - `create_app()` factory (entry point)
- `app/config.py` - environment configuration (`TASKNEST_*`)
- `app/request_id.py` - `X-Request-Id` middleware and request log line
- `app/errors.py` - error envelope handlers
- `app/logging_config.py` - allow-list JSON logging
- `app/routes/meta.py` - health and version
- `tests/` - pytest suite; `check.py` - one-command validation

## Configuration

| Variable | Default | Notes |
|---|---|---|
| `TASKNEST_ENV` | `dev` | `dev`, `test` or `prod`; `/docs` and `/openapi.json` exist only in `dev` |
| `TASKNEST_LOG_LEVEL` | `INFO` | standard level names |
| `TASKNEST_HOST` / `TASKNEST_PORT` | `127.0.0.1` / `8000` | used by the run command below |
| `TASKNEST_MOVED_TO` | unset | optional `https` URL reported as `moved_to` by `/v1/version` |

Invalid values stop startup with `ConfigError`; the rejected value is never printed.

## Run and test (after dependencies are installed)

```
python -m venv out/server/.venv
out/server/.venv/Scripts/python -m pip install -e "server[dev]"
out/server/.venv/Scripts/python server/check.py
cd server && ../out/server/.venv/Scripts/python -m uvicorn --factory app.main:create_app --no-access-log
```

`check.py` never installs packages; it reports missing ones and exits 2. Exit 1 means a check failed.

## Database (M2.1b)

| Variable | Notes |
|---|---|
| `TASKNEST_DATABASE_URL` | application URL (dev, prod). Required in production: `make_engine` refuses to start without it |
| `TASKNEST_MIGRATION_DATABASE_URL` | Alembic URL. dev: falls back to the application URL. test: falls back to `TASKNEST_TEST_DATABASE_URL`, never the dev URL. prod: mandatory, explicit, never falls back |
| `TASKNEST_TEST_DATABASE_URL` | test application and test migration URL; database tests skip when unset |
| `TASKNEST_DATABASE_POOLED` | `false` (default) is an **operator assertion** that the application URL is a direct, non-pooled endpoint; the app cannot verify it. `true` is rejected (pooled endpoints are not supported yet) |

Rules (`app/db/url.py`): driver `postgresql+asyncpg`; no URL query parameters at all (TLS goes through `connect_args`). dev URLs need a local host (`localhost`, `127.0.0.1`, `::1`) and database `tasknest_dev`; test URLs (including an explicit migration URL in test) need a local host and database `tasknest_test`. prod accepts any host, uses `ssl.create_default_context()` (certificate and hostname verified) for both application and migration connections. The prod migration URL must be a direct endpoint; the operator supplies it and nothing verifies it. Errors name only the variable and the rule, never URL parts.

Errors (`app/db/errors.py`, `app/db/session.py`): database failures surface as sanitized `DatabaseError` (phase connect, begin, query, commit; category connection, authentication, resource, canceled, integrity, retryable, query, plus SQLSTATE). Run queries through `session.run()`; application exceptions pass through unchanged. Cleanup failures (rollback, close, invalidate) are logged with fixed fields and never replace the primary exception. `Settings.database_url` is hidden from `repr` only; `asdict()`, `vars()` and attribute access still return it.

```
cd server
alembic upgrade head      # migration 0001: users, devices, refresh_families, refresh_tokens, access_tokens
```

`app/db/tables.py` holds the CLOUD.md 16.6 tables. No auth, recovery or sync tables (0002 is M2.2, 0003 is M3). Server tests need a local PostgreSQL for the database-gated tests; the C++ build and CTest do not.
