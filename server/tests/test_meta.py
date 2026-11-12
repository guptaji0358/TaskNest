from app.config import Settings
from app.main import create_app
from fastapi.testclient import TestClient


def test_app_starts_and_registers_only_meta_routes(app):
    paths = {p for p in app.openapi()["paths"] if p.startswith("/v1")}
    assert paths == {"/v1/health", "/v1/version"}


def test_health(client):
    r = client.get("/v1/health")
    assert r.status_code == 200
    assert r.json() == {"status": "ok"}


def test_version_matches_contract(client):
    body = client.get("/v1/version").json()
    assert body["api"] == "v1"
    assert body["protocol_min"] == body["protocol_max"] == 1
    assert body["limits"] == {"max_ops_per_push": 200, "max_body_bytes": 1048576, "max_pull_limit": 500}
    assert body["moved_to"] is None


def test_version_reports_moved_to():
    settings = Settings(environment="test", log_level="CRITICAL", moved_to="https://new.example.org")
    body = TestClient(create_app(settings)).get("/v1/version").json()
    assert body["moved_to"] == "https://new.example.org"


def test_docs_only_in_dev():
    prod = TestClient(create_app(Settings(environment="prod", log_level="CRITICAL")))
    assert prod.get("/docs").status_code == 404
    assert prod.get("/openapi.json").status_code == 404
