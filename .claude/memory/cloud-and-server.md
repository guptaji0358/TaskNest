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
