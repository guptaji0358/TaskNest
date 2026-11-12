"""Entry point. Run with: uvicorn --factory app.main:create_app --no-access-log

A factory (not a module-level app) so that invalid configuration fails at
startup with a clear ConfigError instead of at import time.
"""
from __future__ import annotations

import os

from fastapi import FastAPI

from . import __version__
from .config import Settings, load_settings
from .errors import install_error_handlers
from .logging_config import configure_logging, get_logger
from .request_id import RequestIdMiddleware
from .routes import meta


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or load_settings(os.environ)
    configure_logging(settings.log_level_no)

    dev = settings.environment == "dev"
    app = FastAPI(
        title="TaskNest Sync API",
        version=__version__,
        # Interactive docs are a dev convenience only.
        docs_url="/docs" if dev else None,
        redoc_url=None,
        openapi_url="/openapi.json" if dev else None,
    )
    app.state.settings = settings
    install_error_handlers(app)
    app.include_router(meta.router)
    app.add_middleware(RequestIdMiddleware)
    get_logger().info("startup", extra={"event": "startup"})
    return app
