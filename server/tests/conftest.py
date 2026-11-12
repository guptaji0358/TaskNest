import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app


@pytest.fixture
def app():
    return create_app(Settings(environment="test", log_level="CRITICAL"))


@pytest.fixture
def client(app):
    # Unhandled exceptions must surface as 500 responses, not test errors.
    return TestClient(app, raise_server_exceptions=False)
