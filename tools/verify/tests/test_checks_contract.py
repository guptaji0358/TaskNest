import json

import pytest

import checks_contract as cc
import core
import repo
from core import Status

OPENAPI = """\
openapi: 3.1.0
info:
  title: T
  version: 1.0.0
tags:
  - name: meta
security:
  - bearerAuth: []
paths:
  /v1/health:
    get:
      operationId: getHealth
      tags: [meta]
      responses:
        "200": { description: ok, content: { application/json: { schema: { $ref: "#/components/schemas/Health" } } } }
  /v1/items/{itemId}:
    parameters:
      - { name: itemId, in: path, required: true, schema: { type: string } }
    delete:
      operationId: deleteItem
      responses:
        "204": { description: gone }
  /v1/sync/push:
    post:
      operationId: syncPush
      responses:
        "200": { description: ok }
components:
  securitySchemes:
    bearerAuth: { type: http, scheme: bearer }
  schemas:
    Health: { type: object }
    CreateOp: { type: object, properties: { type: { type: string, enum: [create] } } }
    DeleteOp: { type: object, properties: { type: { type: string, enum: [delete] } } }
    Op:
      oneOf:
        - { $ref: "#/components/schemas/CreateOp" }
        - { $ref: "#/components/schemas/DeleteOp" }
"""
SYNC_MD = """\
# Sync

## 1. Scope

### 1.1 Terms

`POST /v1/sync/push` sends operations; `GET /v1/health` is public.
"""
README = """\
# Conformance

## `server_scenarios.json`

| Field | Meaning |
|---|---|
| `devices` | map |

## `client_cases.json`

| `kind` | Input | Output | Rule |
|---|---|---|---|
| `outbox_write` | a | b | SYNC 1.1 |
"""
SERVER = {"format": "tasknest-conformance-server/1", "note": "n", "scenarios": [
    {"id": "s1", "rules": ["SYNC 1.1"], "steps": [{"push": {"ops": [{"type": "create"}]}}]}]}
CLIENT = {"format": "tasknest-conformance-client/1", "cases": [
    {"name": "c1", "kind": "outbox_write", "rules": ["SYNC 1"]}]}


@pytest.fixture
def api(fake_repo):
    d = fake_repo / "api"
    (d / "conformance").mkdir(parents=True)
    put(fake_repo, "api/openapi.yaml", OPENAPI)
    put(fake_repo, "api/SYNC.md", SYNC_MD)
    put(fake_repo, "api/conformance/README.md", README)
    put(fake_repo, "api/conformance/server_scenarios.json", json.dumps(SERVER, indent=2) + "\n")
    put(fake_repo, "api/conformance/client_cases.json", json.dumps(CLIENT, indent=2) + "\n")
    return fake_repo


def put(root, rel, text):
    (root / rel).write_bytes(text.encode("utf-8"))


def run_all(root):
    reg = core.Registry()
    cc.register(reg)
    return {r.id: r for r in core.run_plan(core.build_plan(reg, [], False), repo.Ctx(root=root))}


def test_clean_fixture_passes_everything_except_optional_validator(api):
    res = run_all(api)
    for id, r in res.items():
        if id == "openapi_validator":
            assert r.status in (Status.SKIP, Status.PASS)
        else:
            assert r.status is Status.PASS, r


def test_kinds_are_labelled():
    reg = core.Registry()
    cc.register(reg)
    heuristic = {s.id for s in reg.specs() if s.kind == "heuristic"}
    assert heuristic == {"sync_md_endpoints", "conformance_rule_refs", "client_case_kinds"}


def test_missing_api_dir_fails_not_crashes(fake_repo):
    res = run_all(fake_repo)
    assert res["files_present"].status is Status.FAIL
    assert res["yaml_parse"].status is Status.FAIL and res["refs_resolve"].status is Status.FAIL


def test_bad_yaml_and_duplicate_keys_fail(api):
    put(api, "api/openapi.yaml", "openapi: 3.1.0\nopenapi: 3.0.0\n")
    assert "duplicate key" in run_all(api)["yaml_parse"].detail
    put(api, "api/openapi.yaml", "a: [unclosed\n")
    assert run_all(api)["yaml_parse"].status is Status.FAIL


def test_unresolved_ref_fails(api):
    put(api, "api/openapi.yaml", OPENAPI.replace("#/components/schemas/Health", "#/components/schemas/Nope"))
    r = run_all(api)["refs_resolve"]
    assert r.status is Status.FAIL and "Nope" in r.detail


def test_external_ref_fails(api):
    put(api, "api/openapi.yaml", OPENAPI.replace("#/components/schemas/Health", "other.yaml#/Health"))
    assert run_all(api)["refs_resolve"].status is Status.FAIL


def test_pointer_escapes_resolve():
    doc = {"paths": {"/v1/a b": {"x": 1}}, "a~b": 2}
    assert cc.resolve_pointer(doc, "#/paths/~1v1~1a%20b/x") == 1
    assert cc.resolve_pointer(doc, "#/a~0b") == 2
    with pytest.raises(KeyError):
        cc.resolve_pointer(doc, "#/missing")


def test_duplicate_operation_id_fails(api):
    put(api, "api/openapi.yaml", OPENAPI.replace("operationId: deleteItem", "operationId: getHealth"))
    r = run_all(api)["operation_ids"]
    assert r.status is Status.FAIL and "duplicate" in r.detail


def test_missing_operation_id_fails(api):
    put(api, "api/openapi.yaml", OPENAPI.replace("      operationId: syncPush\n", ""))
    assert run_all(api)["operation_ids"].status is Status.FAIL


@pytest.mark.parametrize("old,new,needle", [
    ("openapi: 3.1.0", "openapi: 3.0.3", "expected 3.1.x"),
    ("  title: T\n", "", "info.title"),
    ("/v1/sync/push:", "/sync/push:", "not under /v1/"),
    ("tags: [meta]", "tags: [other]", "undeclared tag"),
    ("- bearerAuth: []", "- missingAuth: []", "undefined scheme"),
])
def test_structure_violations_fail(api, old, new, needle):
    put(api, "api/openapi.yaml", OPENAPI.replace(old, new, 1))
    r = run_all(api)["structure"]
    assert r.status is Status.FAIL and needle in r.detail


def test_path_parameter_mismatch_fails(api):
    put(api, "api/openapi.yaml", OPENAPI.replace("name: itemId", "name: other"))
    assert run_all(api)["path_parameters"].status is Status.FAIL


@pytest.mark.parametrize("data,problem", [
    (b"\xef\xbb\xbfopenapi: 3.1.0\n", "BOM"),
    (b"openapi: 3.1.0\r\n", "CR/CRLF"),
    (b"openapi: \xff\n", "not valid UTF-8"),
    (b"openapi: 3.1.0", "no final newline"),
])
def test_encoding_violations_fail(api, data, problem):
    (api / "api" / "openapi.yaml").write_bytes(data)
    r = run_all(api)["encoding"]
    assert r.status is Status.FAIL and problem in r.detail


def test_conformance_problems_fail(api):
    bad = dict(SERVER, format="wrong")
    put(api, "api/conformance/server_scenarios.json", json.dumps(bad))
    assert run_all(api)["conformance_files"].status is Status.FAIL
    dup = dict(CLIENT, cases=CLIENT["cases"] * 2)
    put(api, "api/conformance/server_scenarios.json", json.dumps(SERVER))
    put(api, "api/conformance/client_cases.json", json.dumps(dup))
    r = run_all(api)["conformance_files"]
    assert r.status is Status.FAIL and "duplicate" in r.detail
    put(api, "api/conformance/client_cases.json", '{"a": 1, "a": 2}')
    assert run_all(api)["conformance_files"].status is Status.FAIL


def test_unknown_op_type_in_fixture_fails(api):
    bad = json.loads(json.dumps(SERVER))
    bad["scenarios"][0]["steps"][0]["push"]["ops"][0]["type"] = "purge"
    put(api, "api/conformance/server_scenarios.json", json.dumps(bad))
    r = run_all(api)["conformance_op_types"]
    assert r.status is Status.FAIL and "purge" in r.detail


def test_sync_md_unknown_endpoint_and_method_are_heuristic_failures(api):
    put(api, "api/SYNC.md", SYNC_MD + "\nSee `/v1/nowhere`.\n")
    r = run_all(api)["sync_md_endpoints"]
    assert r.status is Status.FAIL and r.kind == "heuristic" and "/v1/nowhere" in r.detail
    put(api, "api/SYNC.md", SYNC_MD + "\n`GET /v1/sync/push` is wrong.\n")
    assert "method not declared" in run_all(api)["sync_md_endpoints"].detail


def test_sync_md_prefix_and_template_mentions_are_accepted(api):
    put(api, "api/SYNC.md", SYNC_MD + "\nThe `/v1/sync` family and `DELETE /v1/items/{itemId}`.\n")
    assert run_all(api)["sync_md_endpoints"].status is Status.PASS


def test_unresolved_rule_reference_fails(api):
    bad = json.loads(json.dumps(CLIENT))
    bad["cases"][0]["rules"] = ["SYNC 9.9"]
    put(api, "api/conformance/client_cases.json", json.dumps(bad))
    r = run_all(api)["conformance_rule_refs"]
    assert r.status is Status.FAIL and r.kind == "heuristic" and "9.9" in r.detail


def test_client_case_kind_mismatch_fails(api):
    bad = json.loads(json.dumps(CLIENT))
    bad["cases"][0]["kind"] = "mystery"
    put(api, "api/conformance/client_cases.json", json.dumps(bad))
    r = run_all(api)["client_case_kinds"]
    assert r.status is Status.FAIL and "mystery" in r.detail


def test_validator_skips_when_absent_and_is_never_installed(api, monkeypatch):
    import sys
    monkeypatch.setitem(sys.modules, "openapi_spec_validator", None)  # forces ImportError
    r = run_all(api)["openapi_validator"]
    assert r.status is Status.SKIP and "never installed" in r.detail


def test_validator_result_is_used_when_present(api, monkeypatch):
    import sys
    import types
    fake = types.ModuleType("openapi_spec_validator")
    fake.validate = lambda doc: (_ for _ in ()).throw(ValueError("bad spec"))
    monkeypatch.setitem(sys.modules, "openapi_spec_validator", fake)
    r = run_all(api)["openapi_validator"]
    assert r.status is Status.FAIL and "bad spec" in r.detail
    fake.validate = lambda doc: None
    assert run_all(api)["openapi_validator"].status is Status.PASS


def test_missing_pyyaml_is_incomplete_not_pass(api, monkeypatch):
    monkeypatch.setattr(cc, "_yaml_module", lambda: None)
    cc._parse_yaml.cache_clear()
    try:
        res = run_all(api)
        assert res["yaml_parse"].status is Status.INCOMPLETE
        assert res["refs_resolve"].status is Status.INCOMPLETE
    finally:
        cc._parse_yaml.cache_clear()


def test_contract_files_are_not_modified(api):
    def snap():
        return {p.name: p.read_bytes() for p in (api / "api").rglob("*") if p.is_file()}
    before = snap()
    run_all(api)
    assert snap() == before
