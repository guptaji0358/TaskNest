CODES = {"unauthorized", "forbidden", "not_found", "conflict", "validation_failed", "payload_too_large",
         "rate_limited", "unsupported_protocol", "cursor_ahead", "device_limit_reached",
         "refresh_superseded", "internal"}


def assert_envelope(r, status, code):
    assert r.status_code == status
    assert r.headers["content-type"].startswith("application/json")
    body = r.json()
    assert set(body) == {"error"}
    err = body["error"]
    assert err["code"] == code and err["code"] in CODES
    assert isinstance(err["message"], str) and err["message"]
    assert err["request_id"] == r.headers["X-Request-Id"]


def test_unknown_path_is_not_found(client):
    assert_envelope(client.get("/v1/nope"), 404, "not_found")


def test_wrong_method_keeps_envelope_and_allow(client):
    r = client.post("/v1/health")
    assert_envelope(r, 405, "validation_failed")
    assert "GET" in r.headers["allow"]


def test_validation_error_does_not_echo_input(app, client):
    @app.get("/v1/_probe/{n}")
    async def probe(n: int):
        return {"n": n}

    r = client.get("/v1/_probe/secret-value")
    assert_envelope(r, 422, "validation_failed")
    assert "secret-value" not in r.text


def test_unhandled_exception_is_internal_and_hides_message(app, client):
    @app.get("/v1/_boom")
    async def boom():
        raise RuntimeError("task title: buy milk")

    r = client.get("/v1/_boom")
    assert_envelope(r, 500, "internal")
    assert "buy milk" not in r.text
