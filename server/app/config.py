"""Centralized configuration, loaded from environment variables.

Every setting is optional and has a safe default. An invalid value raises
ConfigError naming the variable, never echoing the rejected value, so a
mistyped secret-like value cannot leak into logs.
"""
from __future__ import annotations

import logging
from collections.abc import Mapping
from dataclasses import dataclass
from urllib.parse import urlsplit

ENVIRONMENTS = ("dev", "test", "prod")
LOG_LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL")

# Advertised by GET /v1/version (api/SYNC.md section 12).
MAX_OPS_PER_PUSH = 200
MAX_BODY_BYTES = 1024 * 1024
MAX_PULL_LIMIT = 500


class ConfigError(ValueError):
    """Missing or invalid configuration."""


@dataclass(frozen=True)
class Settings:
    environment: str = "dev"
    log_level: str = "INFO"
    host: str = "127.0.0.1"
    port: int = 8000
    moved_to: str | None = None

    @property
    def log_level_no(self) -> int:
        return logging.getLevelName(self.log_level)


def load_settings(environ: Mapping[str, str]) -> Settings:
    def get(name: str, default: str) -> str:
        value = environ.get(f"TASKNEST_{name}")
        return default if value is None or value.strip() == "" else value.strip()

    environment = get("ENV", "dev").lower()
    if environment not in ENVIRONMENTS:
        raise ConfigError(f"TASKNEST_ENV must be one of {', '.join(ENVIRONMENTS)}")

    log_level = get("LOG_LEVEL", "INFO").upper()
    if log_level not in LOG_LEVELS:
        raise ConfigError(f"TASKNEST_LOG_LEVEL must be one of {', '.join(LOG_LEVELS)}")

    host = get("HOST", "127.0.0.1")

    port_text = get("PORT", "8000")
    if not port_text.isascii() or not port_text.isdigit() or not 1 <= int(port_text) <= 65535:
        raise ConfigError("TASKNEST_PORT must be an integer from 1 to 65535")

    moved_to = environ.get("TASKNEST_MOVED_TO", "").strip() or None
    if moved_to is not None:
        parts = urlsplit(moved_to)
        if parts.scheme != "https" or not parts.hostname or parts.username or parts.password:
            raise ConfigError("TASKNEST_MOVED_TO must be an https URL without credentials")

    return Settings(environment, log_level, host, int(port_text), moved_to)
