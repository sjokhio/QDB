"""
Milestone 2 tests: full exception hierarchy, ResourceWarning,
locked-database detection, and corrupt-file detection.
"""

import gc
import pathlib
import warnings

import pytest
import qdb


# ---------------------------------------------------------------------------
# Exception hierarchy relationships
# ---------------------------------------------------------------------------

def test_qdberror_is_exception():
    assert issubclass(qdb.QDBError, Exception)


def test_qdbioerror_is_qdberror():
    assert issubclass(qdb.QDBIOError, qdb.QDBError)


def test_qdbcorrupterror_is_qdberror():
    assert issubclass(qdb.QDBCorruptError, qdb.QDBError)


def test_qdbemptyerror_is_qdberror():
    assert issubclass(qdb.QDBEmptyError, qdb.QDBError)


def test_qdbnotfounderror_is_qdberror():
    assert issubclass(qdb.QDBNotFoundError, qdb.QDBError)


def test_qdblockederror_is_qdberror():
    assert issubclass(qdb.QDBLockedError, qdb.QDBError)


def test_all_subclasses_catchable_as_qdberror(db):
    # QDBEmptyError (the most likely to be caught in user code) is a QDBError.
    with pytest.raises(qdb.QDBError):
        db.pop("nonexistent-queue")


# ---------------------------------------------------------------------------
# Locked database
# ---------------------------------------------------------------------------

def test_open_locked_raises_qdblockederror(db_path):
    db1 = qdb.open(db_path)
    try:
        with pytest.raises(qdb.QDBLockedError):
            qdb.open(db_path)
    finally:
        db1.close()


def test_qdblockederror_is_catchable_as_qdberror(db_path):
    db1 = qdb.open(db_path)
    try:
        with pytest.raises(qdb.QDBError):
            qdb.open(db_path)
    finally:
        db1.close()


def test_reopen_succeeds_after_close(db_path):
    db1 = qdb.open(db_path)
    db1.close()
    db2 = qdb.open(db_path)
    db2.close()


# ---------------------------------------------------------------------------
# Corrupt database
# ---------------------------------------------------------------------------

def test_corrupt_file_raises_qdbcorrupterror(tmp_path):
    p = str(tmp_path / "corrupt.qdb")
    pathlib.Path(p).write_bytes(b"not a qdb file" + b"\xff" * 64)
    with pytest.raises(qdb.QDBCorruptError):
        qdb.open(p)


def test_qdbcorrupterror_catchable_as_qdberror(tmp_path):
    p = str(tmp_path / "corrupt.qdb")
    pathlib.Path(p).write_bytes(b"not a qdb file" + b"\xff" * 64)
    with pytest.raises(qdb.QDBError):
        qdb.open(p)


# ---------------------------------------------------------------------------
# ResourceWarning on unclosed handle
# ---------------------------------------------------------------------------

def test_resource_warning_on_unclosed_handle(db_path):
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        db = qdb.open(db_path)
        del db
        gc.collect()

    rw = [w for w in caught if issubclass(w.category, ResourceWarning)]
    assert rw, "expected ResourceWarning for unclosed Database, got none"
    assert "not explicitly closed" in str(rw[0].message)


def test_no_resource_warning_after_explicit_close(db_path):
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        db = qdb.open(db_path)
        db.close()
        del db
        gc.collect()

    rw = [w for w in caught if issubclass(w.category, ResourceWarning)]
    assert not rw, f"unexpected ResourceWarning after close(): {rw}"


def test_no_resource_warning_after_context_manager(db_path):
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        with qdb.open(db_path) as db:
            pass
        del db
        gc.collect()

    rw = [w for w in caught if issubclass(w.category, ResourceWarning)]
    assert not rw, f"unexpected ResourceWarning after 'with' block: {rw}"


# ---------------------------------------------------------------------------
# Input validation — queue name type
# ---------------------------------------------------------------------------

def test_pop_non_str_queue_name(db):
    with pytest.raises(TypeError, match="str"):
        db.pop(123)


def test_push_queue_name_bytes_raises(db):
    with pytest.raises(TypeError, match="str"):
        db.push(b"jobs", b"payload")


# ---------------------------------------------------------------------------
# Unicode queue name round-trip (also covered in test_push_pop but
# kept here as an explicit correctness check)
# ---------------------------------------------------------------------------

def test_unicode_queue_name_round_trip(db):
    name = "队列-αβγ-😀"
    db.push(name, b"unicode queue name test")
    msg = db.pop(name)
    assert msg.queue == name
    assert msg.data == b"unicode queue name test"
    db.ack(msg)


# ---------------------------------------------------------------------------
# Empty payload round-trip
# ---------------------------------------------------------------------------

def test_empty_payload_round_trip(db):
    db.push("q", b"")
    msg = db.pop("q")
    assert msg.data == b""
    assert isinstance(msg.data, bytes)
    db.ack(msg)


# ---------------------------------------------------------------------------
# QDBNotFoundError — stale ack
# qdb_ack returns QDB_ERR_NOENT when msg state is already ACKED.
# ---------------------------------------------------------------------------

def test_double_ack_raises_qdbnotfounderror(db):
    db.push("q", b"task")
    msg = db.pop("q")
    db.ack(msg)
    with pytest.raises(qdb.QDBNotFoundError):
        db.ack(msg)


def test_qdbnotfounderror_catchable_as_qdberror(db):
    db.push("q", b"task")
    msg = db.pop("q")
    db.ack(msg)
    with pytest.raises(qdb.QDBError):
        db.ack(msg)


# ---------------------------------------------------------------------------
# Exception module attributes completeness
# ---------------------------------------------------------------------------

def test_all_exceptions_in_module():
    expected = [
        "QDBError",
        "QDBIOError",
        "QDBCorruptError",
        "QDBEmptyError",
        "QDBNotFoundError",
        "QDBLockedError",
    ]
    for name in expected:
        assert hasattr(qdb, name), f"qdb.{name} not found"
        assert issubclass(getattr(qdb, name), Exception)
