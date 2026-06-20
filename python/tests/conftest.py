import pytest
import qdb


@pytest.fixture
def db(tmp_path):
    """Open a fresh database for each test; close automatically afterwards."""
    with qdb.open(str(tmp_path / "test.qdb")) as h:
        yield h


@pytest.fixture
def db_path(tmp_path):
    """Return a path string for tests that need to open the db themselves."""
    return str(tmp_path / "test.qdb")
