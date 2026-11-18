# Cloud sync and server

Authoritative: `CLOUD.md` (decisions, roadmap §10, M2 plan §16), `api/SYNC.md`, `api/openapi.yaml`,
`api/conformance/`. Summary below is a navigation aid only.

## Status (as recorded in `.claude/CLAUDE.md` and `CLOUD.md`)

- M0 (contract, rules, fixtures, docs): complete.
- M1 (local storage groundwork: schema v4 additive changes, UUIDs, bases, tombstones, outbox,
  conflicts, `SyncStore`): approved and in progress; **no network code exists**.
- M2 (server skeleton, accounts/sessions/recovery/devices only): approved in plan; only the documentation
  step M2.0 is approved so far. M2 contains no sync tables or endpoints.
- Each milestone starts only after explicit owner approval. Do not start the next one, run migrations,
  or install software (PostgreSQL, Docker, Qt kits) without approval.

## Server code (`server/`)

- Implemented: `GET /v1/health`, `GET /v1/version` (`server/app/routes/meta.py`), config (`app/config.py`),
  JSON error envelope (`app/errors.py`), request IDs (`app/request_id.py`), allow-list JSON logging
  (`app/logging_config.py`), factory `create_app()` in `app/main.py`.
- Tests: `server/tests/` (pytest). Validation: `server/check.py` (never installs packages).
- Config variables: `TASKNEST_ENV`, `TASKNEST_LOG_LEVEL`, `TASKNEST_HOST`/`TASKNEST_PORT`, `TASKNEST_MOVED_TO`.
- Venv and caches go under `out/server/`. Server tests are off by default and not part of CTest.
- Not yet implemented: accounts, auth, database, sync. Do not describe them as existing.

## Contract and design invariants (see `CLOUD.md` §5–7 and `api/SYNC.md`)

- Client-generated UUID per task; the server alone orders changes; client clocks never decide.
- SQLite stays the source of truth on clients; sync is off by default and opt-in.
- Deletions are tombstones; edit wins over delete; conflicts are stored and shown.
- Tokens live in the OS secure store, never in SQLite, settings, logs or command lines.
- Provider-neutral: standard PostgreSQL only; no provider SDK or provider-specific fields in `api/`.

## Known open items

- Endpoint rate-limit and trusted-proxy rules (`CLOUD.md` §16.9) are proposed, not approved.
- The OpenAPI changes listed in `CLOUD.md` §16.5 are not approved.

## M2.1b database foundation (implemented, uncommitted, NOT complete until pinned-version gates pass)
- `server/app/db/`: `tables.py` (five 16.6 tables), `url.py` (3-URL policy: app/migration/test; local host + `tasknest_dev`/`tasknest_test` in dev/test; no query params; prod TLS via `connect_args`; `TASKNEST_DATABASE_POOLED=true` rejected), `errors.py` (sanitized `DatabaseError`, specific SQLSTATE categories), `session.py` (`make_engine` refuses missing prod URL, `connection`, `transaction`, `run`). Alembic in `server/migrations/` (0001); env.py uses `migration_url`, no offline mode.
- Design rules: raise translated errors outside `except` blocks (from None does not clear `__context__`); query errors translate only in `run()` because an asynccontextmanager would attach the body exception as `__context__`; cleanup failures are logged, never replace the primary error.
- Tests: `test_db_urls.py`, `test_db_errors.py`, `test_db_alembic.py` (no DB); DB-gated tests skip without `TASKNEST_TEST_DATABASE_URL`. Not yet run against the pinned versions.
