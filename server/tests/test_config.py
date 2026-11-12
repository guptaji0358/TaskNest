import pytest

from app.config import ConfigError, Settings, load_settings
from app.main import create_app


def test_defaults_from_empty_environment():
    assert load_settings({}) == Settings()


def test_values_are_read_and_normalized():
    s = load_settings({
        "TASKNEST_ENV": "PROD", "TASKNEST_LOG_LEVEL": "debug", "TASKNEST_HOST": "0.0.0.0",
        "TASKNEST_PORT": "9000", "TASKNEST_MOVED_TO": "https://new.example.org/",
    })
    assert (s.environment, s.log_level, s.host, s.port) == ("prod", "DEBUG", "0.0.0.0", 9000)
    assert s.moved_to == "https://new.example.org/"


def test_blank_values_use_defaults():
    assert load_settings({"TASKNEST_ENV": "  ", "TASKNEST_PORT": ""}) == Settings()


@pytest.mark.parametrize("env", [
    {"TASKNEST_ENV": "staging"},
    {"TASKNEST_LOG_LEVEL": "verbose"},
    {"TASKNEST_PORT": "0"},
    {"TASKNEST_PORT": "65536"},
    {"TASKNEST_PORT": "80a"},
    {"TASKNEST_PORT": "-1"},
    {"TASKNEST_MOVED_TO": "http://insecure.example.org"},
    {"TASKNEST_MOVED_TO": "https://user:pw@new.example.org"},
    {"TASKNEST_MOVED_TO": "not a url"},
])
def test_invalid_values_are_rejected(env):
    with pytest.raises(ConfigError):
        load_settings(env)


def test_error_never_echoes_the_rejected_value():
    with pytest.raises(ConfigError) as exc:
        load_settings({"TASKNEST_MOVED_TO": "http://user:hunter2@example.org"})
    assert "hunter2" not in str(exc.value)


def test_create_app_fails_clearly_on_invalid_environment(monkeypatch):
    monkeypatch.setenv("TASKNEST_ENV", "bogus")
    with pytest.raises(ConfigError):
        create_app()
