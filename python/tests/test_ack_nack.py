"""Tests for ack(), nack(), and lease semantics."""

import pytest
import qdb


# ---------------------------------------------------------------------------
# ack()
# ---------------------------------------------------------------------------

def test_ack_removes_message(db):
    db.push("q", b"task")
    msg = db.pop("q")
    db.ack(msg)
    # Queue is now empty; a second pop must raise QDBEmptyError.
    with pytest.raises(qdb.QDBEmptyError):
        db.pop("q")


def test_ack_wrong_type_raises(db):
    with pytest.raises(TypeError):
        db.ack("not a message")


def test_ack_none_raises(db):
    with pytest.raises(TypeError):
        db.ack(None)


def test_ack_int_raises(db):
    with pytest.raises(TypeError):
        db.ack(42)


# ---------------------------------------------------------------------------
# nack()
# ---------------------------------------------------------------------------

def test_nack_requeues_message(db):
    db.push("q", b"task")
    msg = db.pop("q")
    db.nack(msg)
    # Message must be available again.
    msg2 = db.pop("q")
    assert msg2.data == b"task"
    db.ack(msg2)


def test_nack_increments_retry_count(db):
    db.push("q", b"task")
    msg = db.pop("q")
    assert msg.retry_count == 0

    db.nack(msg)

    msg2 = db.pop("q")
    assert msg2.retry_count == 1
    db.ack(msg2)


def test_nack_multiple_times(db):
    db.push("q", b"task")

    msg = db.pop("q")
    db.nack(msg)

    msg = db.pop("q")
    db.nack(msg)

    msg = db.pop("q")
    assert msg.retry_count == 2
    db.ack(msg)


def test_nack_wrong_type_raises(db):
    with pytest.raises(TypeError):
        db.nack("not a message")


def test_nack_none_raises(db):
    with pytest.raises(TypeError):
        db.nack(None)


# ---------------------------------------------------------------------------
# Interaction
# ---------------------------------------------------------------------------

def test_ack_and_nack_different_messages(db):
    db.push("q", b"a")
    db.push("q", b"b")

    m1 = db.pop("q")
    m2 = db.pop("q")

    db.ack(m1)
    db.nack(m2)

    # m2 should reappear; m1 should be gone
    m3 = db.pop("q")
    assert m3.data == b"b"
    db.ack(m3)

    with pytest.raises(qdb.QDBEmptyError):
        db.pop("q")
