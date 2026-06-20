"""
qdb — Python bindings for QDB embedded persistent message queue.

Experimental: API may change in v1.3.

Basic usage::

    import qdb

    with qdb.open("work.qdb") as db:
        db.push("jobs", b"payload")
        try:
            msg = db.pop("jobs")
            process(msg.data)
            db.ack(msg)
        except qdb.QDBEmptyError:
            pass  # nothing pending
"""

from qdb._qdb import (   # noqa: F401
    open,
    Database,
    Message,
    QDBError,
    QDBIOError,
    QDBCorruptError,
    QDBEmptyError,
    QDBNotFoundError,
    QDBLockedError,
)

__all__ = [
    "open",
    "Database",
    "Message",
    "QDBError",
    "QDBIOError",
    "QDBCorruptError",
    "QDBEmptyError",
    "QDBNotFoundError",
    "QDBLockedError",
]
