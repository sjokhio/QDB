"""Tests for push(), pop(), and the Message object."""

import pytest
import qdb


# ---------------------------------------------------------------------------
# Payload round-trips
# ---------------------------------------------------------------------------

def test_push_pop_bytes(db):
    db.push("q", b"hello world")
    msg = db.pop("q")
    assert msg.data == b"hello world"


def test_push_pop_empty_payload(db):
    db.push("q", b"")
    msg = db.pop("q")
    assert msg.data == b""


def test_push_pop_bytearray(db):
    db.push("q", bytearray(b"bytearray payload"))
    msg = db.pop("q")
    assert msg.data == b"bytearray payload"


def test_push_pop_memoryview(db):
    src = b"memoryview payload"
    db.push("q", memoryview(src))
    msg = db.pop("q")
    assert msg.data == src


# ---------------------------------------------------------------------------
# Type rejection
# ---------------------------------------------------------------------------

def test_push_str_raises_typeerror(db):
    with pytest.raises(TypeError):
        db.push("q", "text string")


def test_push_int_raises_typeerror(db):
    with pytest.raises(TypeError):
        db.push("q", 42)


# ---------------------------------------------------------------------------
# Queue name validation
# ---------------------------------------------------------------------------

def test_push_empty_queue_name(db):
    with pytest.raises(ValueError, match="empty"):
        db.push("", b"x")


def test_push_long_queue_name(db):
    # 256 ASCII characters → 256 encoded bytes, exceeds QDB_QUEUE_NAME_MAX=255
    with pytest.raises(ValueError, match="too long"):
        db.push("x" * 256, b"x")


def test_push_max_queue_name(db):
    # 255 ASCII characters → exactly QDB_QUEUE_NAME_MAX encoded bytes
    name = "a" * 255
    db.push(name, b"payload")
    msg = db.pop(name)
    assert msg.queue == name
    db.ack(msg)


def test_push_non_str_queue_name(db):
    with pytest.raises(TypeError, match="str"):
        db.push(42, b"x")


# ---------------------------------------------------------------------------
# Empty pop
# ---------------------------------------------------------------------------

def test_pop_empty_raises_qdberror(db):
    with pytest.raises(qdb.QDBEmptyError):
        db.pop("q")


def test_qdbemptyerror_is_qdberror(db):
    with pytest.raises(qdb.QDBError):
        db.pop("noqueue")


# ---------------------------------------------------------------------------
# Message fields and immutability
# ---------------------------------------------------------------------------

def test_message_queue_name(db):
    db.push("myjobs", b"x")
    msg = db.pop("myjobs")
    assert msg.queue == "myjobs"


def test_message_data_is_bytes(db):
    db.push("q", b"payload")
    msg = db.pop("q")
    assert isinstance(msg.data, bytes)


def test_message_id_is_int(db):
    db.push("q", b"x")
    msg = db.pop("q")
    assert isinstance(msg.id, int)
    assert msg.id > 0


def test_message_lease_id_is_int(db):
    db.push("q", b"x")
    msg = db.pop("q")
    assert isinstance(msg.lease_id, int)
    assert msg.lease_id > 0


def test_message_retry_count_is_int(db):
    db.push("q", b"x")
    msg = db.pop("q")
    assert isinstance(msg.retry_count, int)
    assert msg.retry_count == 0


def test_message_id_read_only(db):
    db.push("q", b"x")
    msg = db.pop("q")
    with pytest.raises(AttributeError):
        msg.id = 999


def test_message_data_read_only(db):
    db.push("q", b"x")
    msg = db.pop("q")
    with pytest.raises(AttributeError):
        msg.data = b"y"


def test_message_queue_read_only(db):
    db.push("q", b"x")
    msg = db.pop("q")
    with pytest.raises(AttributeError):
        msg.queue = "other"


# ---------------------------------------------------------------------------
# FIFO ordering
# ---------------------------------------------------------------------------

def test_fifo_order(db):
    db.push("q", b"first")
    db.push("q", b"second")
    db.push("q", b"third")

    m1 = db.pop("q")
    m2 = db.pop("q")
    m3 = db.pop("q")

    assert m1.data == b"first"
    assert m2.data == b"second"
    assert m3.data == b"third"

    db.ack(m1)
    db.ack(m2)
    db.ack(m3)


# ---------------------------------------------------------------------------
# Unicode queue name
# ---------------------------------------------------------------------------

def test_unicode_queue_name(db):
    name = "αβγ"
    db.push(name, b"data")
    msg = db.pop(name)
    assert msg.queue == name
    db.ack(msg)
