"""Error envelope from api/openapi.yaml: {"error": {code, message, request_id}}.

Messages are fixed text per code; they never echo request content.
"""
from __future__ import annotations

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

from .request_id import current_request_id

# HTTP status -> contract error code (the Error.code enum in openapi.yaml).
_STATUS_CODES = {
    401: "unauthorized",
    403: "forbidden",
    404: "not_found",
    409: "conflict",
    413: "payload_too_large",
    429: "rate_limited",
}
_MESSAGES = {
    "unauthorized": "Authentication is required.",
    "forbidden": "This action is not allowed.",
    "not_found": "No such resource.",
    "conflict": "The request conflicts with the current state.",
    "validation_failed": "The request is malformed or breaks a limit.",
    "payload_too_large": "The request body is too large.",
    "rate_limited": "Too many requests.",
    "internal": "Internal server error.",
}


def error_body(code: str) -> dict:
    return {"error": {"code": code, "message": _MESSAGES[code], "request_id": current_request_id()}}


def error_response(code: str, status: int, headers: dict | None = None) -> JSONResponse:
    return JSONResponse(error_body(code), status_code=status, headers=headers)


def install_error_handlers(app: FastAPI) -> None:
    @app.exception_handler(StarletteHTTPException)
    async def http_error(_: Request, exc: StarletteHTTPException) -> JSONResponse:
        # Unlisted 4xx (e.g. 405) fall back to validation_failed; 5xx to internal.
        fallback = "internal" if exc.status_code >= 500 else "validation_failed"
        code = _STATUS_CODES.get(exc.status_code, fallback)
        allow = {k: v for k, v in (exc.headers or {}).items() if k.lower() == "allow"}
        return error_response(code, exc.status_code, allow or None)

    @app.exception_handler(RequestValidationError)
    async def validation_error(_: Request, __: RequestValidationError) -> JSONResponse:
        # Pydantic details echo submitted values, so none are returned.
        return error_response("validation_failed", 422)
