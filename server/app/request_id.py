"""Request ID handling and request logging, as one pure-ASGI middleware.

A client-supplied X-Request-Id is kept only if it is 1-64 characters of
[A-Za-z0-9._-]; otherwise a fresh one is generated. Every response, including
errors and unhandled exceptions, carries X-Request-Id.
"""
from __future__ import annotations

import re
import time
import uuid
from contextvars import ContextVar

from starlette.types import ASGIApp, Message, Receive, Scope, Send

from .logging_config import get_logger

HEADER = b"x-request-id"
_VALID = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
_request_id: ContextVar[str] = ContextVar("request_id", default="-")


def current_request_id() -> str:
    return _request_id.get()


class RequestIdMiddleware:
    def __init__(self, app: ASGIApp) -> None:
        self.app = app

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        supplied = dict(scope["headers"]).get(HEADER, b"").decode("latin-1")
        request_id = supplied if _VALID.match(supplied) else uuid.uuid4().hex
        token = _request_id.set(request_id)
        started = time.perf_counter()
        status = 500
        response_started = False

        async def send_with_id(message: Message) -> None:
            nonlocal status, response_started
            if message["type"] == "http.response.start":
                response_started = True
                status = message["status"]
                headers = [(k, v) for k, v in message["headers"] if k.lower() != HEADER]
                headers.append((HEADER, request_id.encode()))
                message = {**message, "headers": headers}
            await send(message)

        log = get_logger()
        try:
            await self.app(scope, receive, send_with_id)
        except Exception as exc:  # last resort; log the type name only, never the message
            log.error("unhandled_exception", extra={"event": "unhandled_exception",
                                                    "request_id": request_id,
                                                    "error_type": type(exc).__name__})
            if not response_started:
                from .errors import error_response  # local import avoids a cycle

                await error_response("internal", 500)(scope, receive, send_with_id)
        finally:
            log.info("request", extra={
                "event": "request",
                "request_id": request_id,
                "method": scope["method"],
                "path": scope["path"],
                "status": status,
                "duration_ms": round((time.perf_counter() - started) * 1000, 2),
            })
            _request_id.reset(token)
