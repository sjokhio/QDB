"""Tests for qdb.open(), close(), and context manager behaviour."""

import pytest
import qdb


def test_open_and_close(db_path):
    db = qdb.open(db_path)
    db.close()


def test_context_manager_usable_inside_block(db_path):
    with qdb.open(db_path) as db:
        db.push("q", b"hello")
        msg = db.pop("q")
        db.ack(msg)


def test_context_manager_closes_on_exit(db_path):
    with qdb.open(db_path) as db:
        pass
    # After the block the handle must be closed.
    assert repr(db) == "<qdb.Database closed>"


def test_close_idempotent(db_path):
    db = qdb.open(db_path)
    db.close()
    db.close()  # must not raise


def test_method_after_close_push(db_path):
    db = qdb.open(db_path)
    db.close()
    with pytest.raises(ValueError, match="closed"):
        db.push("q", b"x")


def test_method_after_close_pop(db_path):
    db = qdb.open(db_path)
    db.close()
    with pytest.raises(ValueError, match="closed"):
        db.pop("q")


def test_method_after_close_ack(db_path):
    db = qdb.open(db_path)
    db.push("q", b"x")
    msg = db.pop("q")
    db.close()
    with pytest.raises(ValueError, match="closed"):
        db.ack(msg)


def test_method_after_close_nack(db_path):
    db = qdb.open(db_path)
    db.push("q", b"x")
    msg = db.pop("q")
    db.close()
    with pytest.raises(ValueError, match="closed"):
        db.nack(msg)
