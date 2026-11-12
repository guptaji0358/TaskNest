"""Structured JSON logging that can only emit allow-listed fields.

Records are rendered from a fixed set of keys, so request bodies, headers,
query strings, tokens, passwords and task content cannot appear even by
accident. Exception messages and tracebacks are never logged (they may embed
user content); only the exception type name is.
"""
from __future__ import annotations

import json
import logging
import sys
from datetime import datetime, timezone

LOGGER_NAME = "tasknest"
ALLOWED_FIELDS = ("event", "request_id", "method", "path", "status", "duration_ms", "error_type")


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        stamp = datetime.fromtimestamp(record.created, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.")
        entry = {"ts": f"{stamp}{int(record.msecs):03d}Z", "level": record.levelname}
        for key in ALLOWED_FIELDS:
            value = getattr(record, key, None)
            if value is not None:
                entry[key] = value
        return json.dumps(entry, separators=(",", ":"), ensure_ascii=True)


def configure_logging(level: int, stream=None) -> logging.Logger:
    logger = logging.getLogger(LOGGER_NAME)
    logger.setLevel(level)
    logger.propagate = False
    for handler in list(logger.handlers):
        logger.removeHandler(handler)
    handler = logging.StreamHandler(stream or sys.stderr)
    handler.setFormatter(JsonFormatter())
    logger.addHandler(handler)
    return logger


def get_logger() -> logging.Logger:
    return logging.getLogger(LOGGER_NAME)
