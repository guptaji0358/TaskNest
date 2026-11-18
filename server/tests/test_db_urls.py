"""URL validation, production TLS, pooling refusal and Settings secrecy limits (no database needed)."""
import dataclasses
import ssl

import pytest

from app.config import ConfigError, Settings, load_settings
from app.db import url as u
from app.db.session import make_engine

USER, PASSWORD, HOST = "zq_user_7f3a", "P4ss-w0rd-XK9QJ7", "db-leak-canary.invalid"
CANARIES = (USER, PASSWORD, HOST)


def mk(host="127.0.0.1", db="tasknest_test", query=""):
    return f"postgresql+asyncpg://{USER}:{PASSWORD}@{host}:5433/{db}{query}"


def no_canary(text):
    return not any(c in text for c in CANARIES)


# --- application URL selection ------------------------------------------------------------------

def test_dev_reads_only_the_application_url():
    env = {u.APP: mk(db="tasknest_dev"), u.TEST: mk()}
    assert u.application_url(env, "dev").database == "tasknest_dev"
    assert u.application_url({}, "dev") is None


def test_test_reads_only_the_test_url_never_the_dev_url():
    assert u.application_url({u.APP: mk(db="tasknest_dev")}, "test") is None
    assert u.application_url({u.APP: mk(db="tasknest_dev"), u.TEST: mk()}, "test").database == "tasknest_test"


def test_prod_application_url_accepts_any_host_and_database():
    assert u.application_url({u.APP: mk(host=HOST, db="app")}, "prod").host == HOST


def test_prod_missing_application_url_is_refused_at_engine_creation():
    assert load_settings({"TASKNEST_ENV": "prod"}).database_url is None  # load_settings stays permissive
    with pytest.raises(u.DatabaseUrlError, match="required in production"):
        make_engine(None, "prod")


def test_make_engine_without_url_is_a_value_error():
    with pytest.raises(ValueError):
        make_engine(None)


# --- migration URL selection ----------------------------------------------------------------------

def test_dev_migration_url_falls_back_to_the_application_url():
    assert u.migration_url({u.APP: mk(db="tasknest_dev")}, "dev").database == "tasknest_dev"
    explicit = {u.APP: mk(db="tasknest_dev"), u.MIGRATION: mk(host="localhost", db="tasknest_dev")}
    assert u.migration_url(explicit, "dev").host == "localhost"


def test_test_migration_url_falls_back_only_to_the_test_url():
    assert u.migration_url({u.TEST: mk()}, "test").database == "tasknest_test"
    with pytest.raises(u.DatabaseUrlError) as exc:
        u.migration_url({u.APP: mk(db="tasknest_dev")}, "test")  # dev URL is never a fallback
    assert str(exc.value) == f"{u.MIGRATION} or {u.TEST} is not set"


@pytest.mark.parametrize("bad", [mk(host=HOST), mk(host="10.0.0.5"), mk(db="tasknest_dev"), mk(db="prod_db")])
def test_explicit_test_migration_url_cannot_bypass_the_test_restrictions(bad):
    with pytest.raises(u.DatabaseUrlError) as exc:
        u.migration_url({u.MIGRATION: bad, u.TEST: mk()}, "test")
    assert no_canary(str(exc.value))


def test_prod_migration_url_is_explicit_and_never_the_application_url():
    with pytest.raises(u.DatabaseUrlError, match=u.MIGRATION):
        u.migration_url({u.APP: mk(host=HOST, db="app")}, "prod")
    assert u.migration_url({u.MIGRATION: mk(host=HOST, db="app")}, "prod").host == HOST


# --- host and database rules --------------------------------------------------------------------

@pytest.mark.parametrize("host", ["localhost", "LOCALHOST", "127.0.0.1", "[::1]"])
def test_local_hosts_are_accepted_in_dev_and_test(host):
    assert u.application_url({u.TEST: mk(host=host)}, "test") is not None
    assert u.application_url({u.APP: mk(host=host, db="tasknest_dev")}, "dev") is not None


@pytest.mark.parametrize("env,url", [
    ("dev", mk(host=HOST, db="tasknest_dev")), ("dev", mk(db="tasknest_test")), ("dev", mk(db="tasknest_dev2")),
    ("test", mk(host="192.168.1.5")), ("test", mk(db="tasknest_dev")),
])
def test_wrong_host_or_database_is_rejected_without_leaking(env, url):
    name = u.TEST if env == "test" else u.APP
    with pytest.raises(u.DatabaseUrlError) as exc:
        u.application_url({name: url}, env)
    assert no_canary(str(exc.value)) and name in str(exc.value)


@pytest.mark.parametrize("text", ["mysql://u:secret@h/db", "postgresql://u:p@h/db", "not a url", "postgresql+asyncpg://",
                                  f"postgresql+asyncpg://{USER}:{PASSWORD}@/db"])
def test_malformed_or_wrong_driver_is_rejected_without_context(text):
    with pytest.raises(u.DatabaseUrlError) as exc:
        u.parse(text, u.APP)
    assert exc.value.__context__ is None and exc.value.__cause__ is None
    assert "secret" not in str(exc.value) and no_canary(str(exc.value))


# --- query parameters and production TLS ----------------------------------------------------------

@pytest.mark.parametrize("query", ["?ssl=true", "?sslmode=disable", "?SSL=1", "?SslMode=require", "?%73sl=1",
                                   "?a=1&ssl=1", "?ssl=", "?application_name=x", "?prepared_statement_cache_size=0"])
@pytest.mark.parametrize("env", ["dev", "test", "prod"])
def test_every_url_query_parameter_is_rejected(env, query):
    db = {"dev": "tasknest_dev", "test": "tasknest_test", "prod": "app"}[env]
    with pytest.raises(u.DatabaseUrlError) as exc:
        u.parse(mk(host="localhost", db=db, query=query), u.APP)
    assert no_canary(str(exc.value))


def test_ssl_parameters_get_a_specific_message():
    with pytest.raises(u.DatabaseUrlError, match="ssl and sslmode"):
        u.parse(mk(query="?sslmode=disable"), u.APP)


def test_production_tls_context_verifies_certificate_and_hostname():
    ctx = u.connect_args("prod")["ssl"]
    assert isinstance(ctx, ssl.SSLContext)
    assert ctx.verify_mode == ssl.CERT_REQUIRED and ctx.check_hostname is True
    assert u.connect_args("dev") == {} and u.connect_args("test") == {}


@pytest.mark.parametrize("env", ["dev", "test", "prod"])
def test_make_engine_passes_connect_args_and_hides_parameters(monkeypatch, env):
    seen = {}
    monkeypatch.setattr("app.db.session.create_async_engine", lambda url, **kw: seen.update(url=url, **kw) or "engine")
    url = u.parse(mk(host=HOST, db="app"), u.APP)
    assert make_engine(url, env) == "engine"
    assert seen["url"] is url  # a real URL object, never rendered to text
    assert seen["hide_parameters"] is True and ("ssl" in seen["connect_args"]) == (env == "prod")


# --- pooling ----------------------------------------------------------------------------------------

def test_pooled_default_is_false_and_true_is_rejected():
    assert u.parse_pooled(None) is False and u.parse_pooled("false") is False and u.parse_pooled(" FALSE ") is False
    with pytest.raises(u.DatabaseUrlError, match="pooled endpoints are not supported yet"):
        u.parse_pooled("true")
    assert load_settings({"TASKNEST_DATABASE_POOLED": "false"}) == Settings()
    with pytest.raises(ConfigError, match="not supported yet"):
        load_settings({"TASKNEST_DATABASE_POOLED": "true"})


@pytest.mark.parametrize("value", ["yes", "1", "no", "tru"])
def test_pooled_other_values_are_rejected(value):
    with pytest.raises(ConfigError, match="must be true or false"):
        load_settings({"TASKNEST_DATABASE_POOLED": value})


# --- Settings secrecy: repr only -------------------------------------------------------------------

def test_settings_repr_omits_the_url_but_the_value_stays_accessible():
    s = load_settings({"TASKNEST_ENV": "test", u.TEST: mk()})
    assert no_canary(repr(s)) and no_canary(str(s)) and no_canary(f"{s}")
    # Documented limit: repr=False is not protection. These still return the URL object.
    assert dataclasses.asdict(s)["database_url"] is not None
    assert vars(s)["database_url"].password == PASSWORD
    assert s.database_url.password == PASSWORD


def test_config_errors_never_echo_the_url():
    with pytest.raises(ConfigError) as exc:
        load_settings({"TASKNEST_ENV": "test", u.TEST: mk(host=HOST)})
    assert no_canary(str(exc.value)) and exc.value.__context__ is None and exc.value.__cause__ is None
