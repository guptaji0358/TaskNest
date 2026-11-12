import io
import json
import logging

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.logging_config import ALLOWED_FIELDS, JsonFormatter, LOGGER_NAME
from app.main import create_app

SECRETS = ["hunter2-password", "tok_live_SECRET", "buy milk and eggs", "refresh-abc-SECRET"]


@pytest.fixture
def log_stream():
    app = create_app(Settings(environment="test", log_level="DEBUG"))
    stream = io.StringIO()
    handler = logging.StreamHandler(stream)
    handler.setFormatter(JsonFormatter())
    logging.getLogger(LOGGER_NAME).addHandler(handler)
    yield app, stream
    logging.getLogger(LOGGER_NAME).removeHandler(handler)


def lines(stream):
    return [json.loads(line) for line in stream.getvalue().splitlines()]


def test_request_is_logged_with_allowed_fields_only(log_stream):
    app, stream = log_stream
    TestClient(app).get("/v1/health", headers={"X-Request-Id": "req-9"})
    entry = [e for e in lines(stream) if e.get("event") == "request"][-1]
    assert entry["request_id"] == "req-9" and entry["status"] == 200
    assert entry["method"] == "GET" and entry["path"] == "/v1/health"
    assert set(entry) <= set(ALLOWED_FIELDS) | {"ts", "level"}


def test_sensitive_values_never_reach_the_log(log_stream):
    app, stream = log_stream

    @app.post("/v1/_probe")
    async def probe(data: dict):
        raise RuntimeError(data["title"])

    client = TestClient(app, raise_server_exceptions=False)
    client.get("/v1/health", headers={"Authorization": "Bearer tok_live_SECRET"},
               params={"password": "hunter2-password"})
    client.post("/v1/_probe", json={"title": "buy milk and eggs", "refresh": "refresh-abc-SECRET"},
                headers={"Authorization": "Bearer tok_live_SECRET", "Cookie": "s=hunter2-password"})
    output = stream.getvalue()
    assert output
    for secret in SECRETS:
        assert secret not in output
    assert any(e.get("event") == "unhandled_exception" and e["error_type"] == "RuntimeError"
               for e in lines(stream))
