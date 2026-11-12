import re

HEX32 = re.compile(r"^[0-9a-f]{32}$")


def test_generated_when_absent(client):
    r = client.get("/v1/health")
    assert HEX32.match(r.headers["X-Request-Id"])


def test_valid_client_id_is_echoed(client):
    r = client.get("/v1/health", headers={"X-Request-Id": "abc-123_X.y"})
    assert r.headers["X-Request-Id"] == "abc-123_X.y"


def test_invalid_client_id_is_replaced(client):
    for bad in ("has space", "x" * 65, "semi;colon", "new\tline"):
        r = client.get("/v1/health", headers={"X-Request-Id": bad})
        assert HEX32.match(r.headers["X-Request-Id"]), bad


def test_ids_differ_between_requests(client):
    ids = {client.get("/v1/health").headers["X-Request-Id"] for _ in range(5)}
    assert len(ids) == 5


def test_error_body_carries_same_id_as_header(client):
    r = client.get("/v1/nope", headers={"X-Request-Id": "req-1"})
    assert r.headers["X-Request-Id"] == "req-1"
    assert r.json()["error"]["request_id"] == "req-1"
