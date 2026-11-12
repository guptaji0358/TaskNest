"""Group "contract": checks on api/openapi.yaml, api/SYNC.md and api/conformance/ (Step 3.3).

Deterministic checks parse structured data (YAML, JSON) or inspect bytes. Heuristic checks
scan prose with regular expressions; they are registered with kind="heuristic" and are
labelled as such in every report. Nothing here judges whether the sync or conflict rules are
*correct*: a pass means the documents are well-formed and refer to each other consistently.
Read-only: no file is written and nothing is installed.
"""
from __future__ import annotations

import functools
import json
import re
from pathlib import Path
from urllib.parse import unquote

from core import KIND_DETERMINISTIC, KIND_HEURISTIC, Registry, Result, Status

GROUP = "contract"
HTTP_METHODS = ("get", "put", "post", "delete", "patch", "head", "options", "trace")
PATH_ITEM_KEYS = set(HTTP_METHODS) | {"summary", "description", "servers", "parameters", "$ref"}
OPERATION_ID_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")
CONFORMANCE_FORMATS = {"server_scenarios.json": ("tasknest-conformance-server/1", "scenarios"),
                       "client_cases.json": ("tasknest-conformance-client/1", "cases")}
MAX_LISTED = 8


def _res(id: str, status: Status, detail: str = "", kind: str = KIND_DETERMINISTIC) -> Result:
    return Result(GROUP, id, status, detail, kind)


def _listed(items) -> str:
    items = sorted(set(items))
    more = f" (+{len(items) - MAX_LISTED} more)" if len(items) > MAX_LISTED else ""
    return ", ".join(items[:MAX_LISTED]) + more


def _api(ctx, *parts: str) -> Path:
    return ctx.path("api", *parts)


# --- loading -------------------------------------------------------------------------------

def _yaml_module():
    try:
        import yaml  # PyYAML; optional, never installed by this toolkit
        return yaml
    except ImportError:
        return None


def _strict_loader(yaml):
    class Loader(yaml.SafeLoader):
        pass

    def construct(loader, node, deep=False):
        seen = set()
        for key_node, _ in node.value:
            key = loader.construct_object(key_node, deep=True)
            if key in seen:
                raise yaml.constructor.ConstructorError(
                    None, None, f"duplicate key {key!r}", key_node.start_mark)
            seen.add(key)
        return yaml.SafeLoader.construct_mapping(loader, node, deep)

    Loader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, construct)
    return Loader


@functools.lru_cache(maxsize=8)
def _parse_yaml(path: str, mtime_ns: int, size: int):
    """Returns (document, error). `mtime_ns`/`size` only key the cache."""
    yaml = _yaml_module()
    if yaml is None:
        return None, "PyYAML is not installed"
    try:
        text = Path(path).read_bytes().decode("utf-8")
        return yaml.load(text, Loader=_strict_loader(yaml)), None
    except Exception as exc:  # noqa: BLE001 - any parser error is a finding, not a crash
        return None, f"{type(exc).__name__}: {str(exc).splitlines()[0] if str(exc) else ''}"[:240]


def _load_openapi(ctx):
    path = _api(ctx, "openapi.yaml")
    if not path.is_file():
        return None, "api/openapi.yaml not found"
    st = path.stat()
    return _parse_yaml(str(path), st.st_mtime_ns, st.st_size)


def _need_doc(ctx):
    """(doc, None) or (None, Result) with the right non-PASS status for the calling check."""
    doc, err = _load_openapi(ctx)
    if doc is None:
        status = Status.INCOMPLETE if err == "PyYAML is not installed" else Status.FAIL
        return None, status, f"openapi.yaml unavailable: {err}"
    if not isinstance(doc, dict):
        return None, Status.FAIL, "openapi.yaml top level is not a mapping"
    return doc, None, ""


# --- deterministic: files, encoding, parsing ------------------------------------------------

def _contract_files(ctx) -> list[Path]:
    api = _api(ctx)
    files = [api / "openapi.yaml", api / "SYNC.md"]
    conf = api / "conformance"
    if conf.is_dir():
        files += sorted(p for p in conf.iterdir() if p.suffix in (".json", ".md"))
    return files


def check_files_present(ctx):
    required = [("openapi.yaml",), ("SYNC.md",), ("conformance", "README.md")] + \
               [("conformance", n) for n in CONFORMANCE_FORMATS]
    missing = ["api/" + "/".join(p) for p in required if not _api(ctx, *p).is_file()]
    if missing:
        return _res("files_present", Status.FAIL, "missing: " + _listed(missing))
    return _res("files_present", Status.PASS, f"{len(required)} required contract files exist")


def check_encoding(ctx):
    problems = []
    files = _contract_files(ctx)
    for path in files:
        if not path.is_file():
            continue
        data = path.read_bytes()
        rel = path.relative_to(ctx.root).as_posix()
        if data.startswith(b"\xef\xbb\xbf"):
            problems.append(f"{rel}: BOM")
        if b"\r" in data:
            problems.append(f"{rel}: CR/CRLF line ending")
        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            problems.append(f"{rel}: not valid UTF-8")
        if data and not data.endswith(b"\n"):
            problems.append(f"{rel}: no final newline")
    if problems:
        return _res("encoding", Status.FAIL, _listed(problems))
    return _res("encoding", Status.PASS, f"{len(files)} files are LF, UTF-8, no BOM")


def check_yaml_parse(ctx):
    doc, status, detail = _need_doc(ctx)
    if doc is None:
        return _res("yaml_parse", status, detail)
    return _res("yaml_parse", Status.PASS, "parsed as YAML with no duplicate keys")


def _walk(node, path=""):
    yield path, node
    if isinstance(node, dict):
        for key, value in node.items():
            yield from _walk(value, f"{path}/{key}")
    elif isinstance(node, list):
        for i, value in enumerate(node):
            yield from _walk(value, f"{path}/{i}")


def resolve_pointer(doc, ref: str):
    """Resolve a local `#/a/b` reference. Raises KeyError if it does not resolve."""
    if not ref.startswith("#"):
        raise KeyError("external reference")
    pointer = unquote(ref[1:])
    node = doc
    if pointer == "":
        return node
    if not pointer.startswith("/"):
        raise KeyError("bad pointer")
    for part in pointer[1:].split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        if isinstance(node, dict) and part in node:
            node = node[part]
        elif isinstance(node, list) and part.isdigit() and int(part) < len(node):
            node = node[int(part)]
        else:
            raise KeyError(part)
    return node


def check_refs(ctx):
    doc, status, detail = _need_doc(ctx)
    if doc is None:
        return _res("refs_resolve", status, detail)
    bad, total = [], 0
    for where, node in _walk(doc):
        if isinstance(node, dict) and "$ref" in node:
            total += 1
            ref = node["$ref"]
            try:
                if not isinstance(ref, str):
                    raise KeyError("not a string")
                resolve_pointer(doc, ref)
            except KeyError:
                bad.append(f"{ref!r} at {where or '/'}")
    if bad:
        return _res("refs_resolve", Status.FAIL, f"{len(bad)} unresolved of {total}: " + _listed(bad))
    return _res("refs_resolve", Status.PASS, f"{total} $ref values all resolve locally")


def _operations(doc):
    """Yield (path, method, operation) for every operation object."""
    for path, item in (doc.get("paths") or {}).items():
        if isinstance(item, dict):
            for method in HTTP_METHODS:
                if isinstance(item.get(method), dict):
                    yield path, method, item[method]


def check_operation_ids(ctx):
    doc, status, detail = _need_doc(ctx)
    if doc is None:
        return _res("operation_ids", status, detail)
    seen, problems = {}, []
    for path, method, op in _operations(doc):
        oid, where = op.get("operationId"), f"{method.upper()} {path}"
        if not isinstance(oid, str) or not OPERATION_ID_RE.match(oid):
            problems.append(f"{where}: missing or invalid operationId")
        elif oid in seen:
            problems.append(f"{oid}: duplicate ({seen[oid]} and {where})")
        else:
            seen[oid] = where
    if problems:
        return _res("operation_ids", Status.FAIL, _listed(problems))
    return _res("operation_ids", Status.PASS, f"{len(seen)} operationIds, all unique")


def _structure_problems(doc) -> list[str]:
    problems = []
    for key in ("openapi", "info", "paths", "components"):
        if key not in doc:
            problems.append(f"missing top-level '{key}'")
    if not str(doc.get("openapi", "")).startswith("3.1."):
        problems.append(f"openapi version is {doc.get('openapi')!r}, expected 3.1.x")
    info = doc.get("info") if isinstance(doc.get("info"), dict) else {}
    for key in ("title", "version"):
        if not info.get(key):
            problems.append(f"info.{key} missing")
    paths = doc.get("paths")
    if not isinstance(paths, dict) or not paths:
        problems.append("paths is empty or not a mapping")
        return problems
    components = doc.get("components") if isinstance(doc.get("components"), dict) else {}
    schemes = set((components.get("securitySchemes") or {}))
    declared_tags = {t.get("name") for t in doc.get("tags") or [] if isinstance(t, dict)}
    for req in doc.get("security") or []:
        problems += [f"global security uses undefined scheme {n}" for n in req if n not in schemes]
    for path, item in paths.items():
        if not str(path).startswith("/v1/"):
            problems.append(f"{path}: not under /v1/")
        if not isinstance(item, dict):
            problems.append(f"{path}: path item is not a mapping")
            continue
        problems += [f"{path}: unknown path-item key '{k}'" for k in item if k not in PATH_ITEM_KEYS]
    for path, method, op in _operations(doc):
        where = f"{method.upper()} {path}"
        if not isinstance(op.get("responses"), dict) or not op["responses"]:
            problems.append(f"{where}: no responses")
        problems += [f"{where}: undeclared tag {t}" for t in op.get("tags") or [] if t not in declared_tags]
        for req in op.get("security") or []:
            problems += [f"{where}: undefined security scheme {n}" for n in req if n not in schemes]
    return problems


def check_structure(ctx):
    doc, status, detail = _need_doc(ctx)
    if doc is None:
        return _res("structure", status, detail)
    problems = _structure_problems(doc)
    if problems:
        return _res("structure", Status.FAIL, _listed(problems))
    return _res("structure", Status.PASS, f"{len(list(_operations(doc)))} operations under {len(doc['paths'])} paths")


def check_path_parameters(ctx):
    doc, status, detail = _need_doc(ctx)
    if doc is None:
        return _res("path_parameters", status, detail)

    def names(params):
        out = set()
        for p in params or []:
            try:
                p = resolve_pointer(doc, p["$ref"]) if isinstance(p, dict) and "$ref" in p else p
            except KeyError:
                continue  # reported by contract.refs_resolve
            if isinstance(p, dict) and p.get("in") == "path" and p.get("required") is True:
                out.add(p.get("name"))
        return out

    problems = []
    for path, method, op in _operations(doc):
        declared = names(doc["paths"][path].get("parameters")) | names(op.get("parameters"))
        needed = set(re.findall(r"\{([^}]+)\}", path))
        if needed != declared:
            problems.append(f"{method.upper()} {path}: template {sorted(needed)} vs required path params {sorted(declared)}")
    if problems:
        return _res("path_parameters", Status.FAIL, _listed(problems))
    return _res("path_parameters", Status.PASS, "path templates match declared required path parameters")


def check_openapi_validator(ctx):
    try:
        import openapi_spec_validator as osv
    except ImportError:
        return _res("openapi_validator", Status.SKIP,
                    "optional openapi-spec-validator is not installed (never installed by this toolkit)")
    doc, status, detail = _need_doc(ctx)
    if doc is None:
        return _res("openapi_validator", status, detail)
    try:
        osv.validate(doc)
    except Exception as exc:  # noqa: BLE001
        return _res("openapi_validator", Status.FAIL, f"{type(exc).__name__}: {str(exc)[:200]}")
    return _res("openapi_validator", Status.PASS, "openapi-spec-validator accepted the document")


# --- conformance fixtures (deterministic) ---------------------------------------------------

def _no_dupes(pairs):
    keys = [k for k, _ in pairs]
    dup = {k for k in keys if keys.count(k) > 1}
    if dup:
        raise ValueError(f"duplicate JSON key {sorted(dup)[0]!r}")
    return dict(pairs)


def _load_json(path: Path):
    try:
        return json.loads(path.read_bytes().decode("utf-8"), object_pairs_hook=_no_dupes), None
    except Exception as exc:  # noqa: BLE001
        return None, f"{path.name}: {type(exc).__name__}: {str(exc)[:120]}"


def _fixtures(ctx):
    """Returns ({filename: (format, items)}, [problems])."""
    out, problems = {}, []
    for name, (fmt, key) in CONFORMANCE_FORMATS.items():
        path = _api(ctx, "conformance", name)
        if not path.is_file():
            problems.append(f"{name}: missing")
            continue
        data, err = _load_json(path)
        if err:
            problems.append(err)
        elif not isinstance(data, dict) or data.get("format") != fmt:
            problems.append(f"{name}: format is not {fmt!r}")
        elif not isinstance(data.get(key), list) or not data[key]:
            problems.append(f"{name}: '{key}' is empty or not a list")
        else:
            out[name] = (fmt, data[key])
    return out, problems


def check_conformance_files(ctx):
    fixtures, problems = _fixtures(ctx)
    for name, (_, items) in fixtures.items():
        idkey = "id" if name == "server_scenarios.json" else "name"
        ids = [i.get(idkey) if isinstance(i, dict) else None for i in items]
        if any(not isinstance(i, str) or not i for i in ids):
            problems.append(f"{name}: an entry has no string '{idkey}'")
        dups = {i for i in ids if i is not None and ids.count(i) > 1}
        if dups:
            problems.append(f"{name}: duplicate {idkey}: {_listed(dups)}")
    if problems:
        return _res("conformance_files", Status.FAIL, _listed(problems))
    counts = ", ".join(f"{n} {len(v[1])}" for n, v in sorted(fixtures.items()))
    return _res("conformance_files", Status.PASS, f"valid JSON, formats and unique ids ({counts})")


def _op_types(doc):
    types = set()
    for entry in doc["components"]["schemas"]["Op"]["oneOf"]:
        schema = resolve_pointer(doc, entry["$ref"])
        types.update(schema["properties"]["type"]["enum"])
    return types


def check_conformance_op_types(ctx):
    doc, status, detail = _need_doc(ctx)
    if doc is None:
        return _res("conformance_op_types", status, detail)
    fixtures, problems = _fixtures(ctx)
    if "server_scenarios.json" not in fixtures:
        return _res("conformance_op_types", Status.FAIL, "server_scenarios.json unusable: " + _listed(problems))
    try:
        allowed = _op_types(doc)
    except (KeyError, TypeError):
        return _res("conformance_op_types", Status.FAIL, "cannot derive operation types from the Op schema")
    unknown = set()
    for scenario in fixtures["server_scenarios.json"][1]:
        for step in scenario.get("steps", []):
            for op in (step.get("push") or {}).get("ops", []):
                if op.get("type") not in allowed:
                    unknown.add(str(op.get("type")))
    if unknown:
        return _res("conformance_op_types", Status.FAIL, "operation types not in the Op schema: " + _listed(unknown))
    return _res("conformance_op_types", Status.PASS,
                f"every pushed operation type is one of {sorted(allowed)} (structural only)")


# --- cross-references: heuristic (prose scanned with regular expressions) -------------------

def _template_regex(path: str):
    return re.compile("^" + re.sub(r"\\\{[^}]*\\\}", r"[^/]+", re.escape(path)) + "$")


def _known_path(doc_paths, mention: str):
    for path in doc_paths:
        if _template_regex(path).match(mention):
            return path
    return None


_MENTION_RE = re.compile(r"(?:\b(GET|POST|PUT|DELETE|PATCH)\s+)?`?(/v1/[A-Za-z0-9_\-/{}]*[A-Za-z0-9_}])")


def check_sync_md_endpoints(ctx):
    doc, status, detail = _need_doc(ctx)
    kind = KIND_HEURISTIC
    if doc is None:
        return _res("sync_md_endpoints", status, detail, kind)
    path = _api(ctx, "SYNC.md")
    if not path.is_file():
        return _res("sync_md_endpoints", Status.FAIL, "api/SYNC.md not found", kind)
    paths = doc.get("paths") or {}
    problems, mentions = [], 0
    for line in path.read_text(encoding="utf-8").splitlines():
        for match in _MENTION_RE.finditer(line):
            mentions += 1
            method, mention = (match.group(1) or "").lower(), match.group(2)
            hit = _known_path(paths, mention)
            if hit is None:
                # A bare prefix of a real path (for example /v1/sync) is a legitimate mention.
                if not any(p.startswith(mention + "/") for p in paths):
                    problems.append(f"{mention}: not in openapi.yaml")
            elif method and method not in paths[hit]:
                problems.append(f"{method.upper()} {mention}: method not declared")
    if problems:
        return _res("sync_md_endpoints", Status.FAIL, _listed(problems), kind)
    return _res("sync_md_endpoints", Status.PASS,
                f"{mentions} endpoint mentions in SYNC.md exist in openapi.yaml (regex scan)", kind)


def _sync_sections(ctx) -> set:
    text = _api(ctx, "SYNC.md").read_text(encoding="utf-8")
    return set(re.findall(r"^#{2,4}\s+(\d+(?:\.\d+)*)\.?\s", text, re.M))


def check_conformance_rule_refs(ctx):
    kind = KIND_HEURISTIC
    fixtures, problems = _fixtures(ctx)
    if problems or not _api(ctx, "SYNC.md").is_file():
        return _res("conformance_rule_refs", Status.FAIL, "fixtures or SYNC.md unusable: " + _listed(problems), kind)
    sections, bad, total = _sync_sections(ctx), [], 0
    for name, (_, items) in fixtures.items():
        for item in items:
            for rule in item.get("rules", []) if isinstance(item, dict) else []:
                total += 1
                match = re.fullmatch(r"SYNC (\d+(?:\.\d+)*)", str(rule))
                if not match or match.group(1) not in sections:
                    bad.append(f"{name}: {rule!r}")
    if bad:
        return _res("conformance_rule_refs", Status.FAIL, f"{len(bad)} of {total} unresolved: " + _listed(bad), kind)
    return _res("conformance_rule_refs", Status.PASS,
                f"{total} fixture rule references match SYNC.md section numbers (heading scan)", kind)


def check_client_case_kinds(ctx):
    kind = KIND_HEURISTIC
    fixtures, problems = _fixtures(ctx)
    readme = _api(ctx, "conformance", "README.md")
    if "client_cases.json" not in fixtures or not readme.is_file():
        return _res("client_case_kinds", Status.FAIL, "client_cases.json or README.md unusable", kind)
    used = {c.get("kind") for c in fixtures["client_cases.json"][1] if isinstance(c, dict)}
    text = readme.read_text(encoding="utf-8")
    section = re.search(r"^## `client_cases\.json`.*?(?=^## |\Z)", text, re.M | re.S)
    # Only the client_cases section; its header row names the column `kind`, which is not a case kind.
    documented = set(re.findall(r"^\| `([a-z_]+)` \|", section.group(0) if section else "", re.M)) - {"kind"}
    problems = [f"{k}: used but not documented" for k in used - documented] + \
               [f"{k}: documented but unused" for k in documented - used]
    if problems:
        return _res("client_case_kinds", Status.FAIL, _listed(problems), kind)
    return _res("client_case_kinds", Status.PASS,
                f"{len(used)} client case kinds match the README table (table-row scan)", kind)


# --- registration ----------------------------------------------------------------------------

CHECKS = (
    ("files_present", check_files_present, KIND_DETERMINISTIC),
    ("encoding", check_encoding, KIND_DETERMINISTIC),
    ("yaml_parse", check_yaml_parse, KIND_DETERMINISTIC),
    ("refs_resolve", check_refs, KIND_DETERMINISTIC),
    ("operation_ids", check_operation_ids, KIND_DETERMINISTIC),
    ("structure", check_structure, KIND_DETERMINISTIC),
    ("path_parameters", check_path_parameters, KIND_DETERMINISTIC),
    ("openapi_validator", check_openapi_validator, KIND_DETERMINISTIC),
    ("conformance_files", check_conformance_files, KIND_DETERMINISTIC),
    ("conformance_op_types", check_conformance_op_types, KIND_DETERMINISTIC),
    ("sync_md_endpoints", check_sync_md_endpoints, KIND_HEURISTIC),
    ("conformance_rule_refs", check_conformance_rule_refs, KIND_HEURISTIC),
    ("client_case_kinds", check_client_case_kinds, KIND_HEURISTIC),
)


def register(registry: Registry) -> None:
    for id, func, kind in CHECKS:
        registry.register(GROUP, id, func, kind=kind)
