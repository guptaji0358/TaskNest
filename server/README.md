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
