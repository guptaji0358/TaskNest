"""Unauthenticated meta endpoints: getHealth and getVersion (api/openapi.yaml)."""
from __future__ import annotations

from fastapi import APIRouter, Request

from ..config import MAX_BODY_BYTES, MAX_OPS_PER_PUSH, MAX_PULL_LIMIT

router = APIRouter(prefix="/v1", tags=["meta"])


@router.get("/health", operation_id="getHealth")
async def get_health() -> dict:
    return {"status": "ok"}


@router.get("/version", operation_id="getVersion")
async def get_version(request: Request) -> dict:
    return {
        "api": "v1",
        "protocol_min": 1,
        "protocol_max": 1,
        "limits": {
            "max_ops_per_push": MAX_OPS_PER_PUSH,
            "max_body_bytes": MAX_BODY_BYTES,
            "max_pull_limit": MAX_PULL_LIMIT,
        },
        "moved_to": request.app.state.settings.moved_to,
    }
