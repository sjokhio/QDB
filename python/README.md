# qdb-embedded — Python bindings for QDB

> **Experimental.** The Python API may change in v1.3.  
> Requires Python ≥ 3.9 and a C17-capable compiler.

## Install (from repo root)

```sh
pip install -e python/
```

No prior CMake build is required. The extension compiles the QDB C sources
directly from `src/` in the parent repository.

## Quick start

```python
import qdb

with qdb.open("work.qdb") as db:
    # Push — payload must be bytes-like (bytes, bytearray, memoryview)
    db.push("jobs", b"resize-image-42.jpg")

    try:
        msg = db.pop("jobs")
        print(msg.queue, msg.data)   # "jobs", b"resize-image-42.jpg"

        process(msg.data)
        db.ack(msg)          # permanent removal
    except qdb.QDBEmptyError:
        pass                 # nothing pending in this queue
```

## API

### `qdb.open(path) → Database`

Open or create the queue database at `path`.  
Returns a `Database`.  Use as a context manager so it is closed reliably.

Raises `QDBLockedError` if another handle to the same file is already open.  
Raises `QDBCorruptError` if the file header is damaged.  
Raises `QDBIOError` for OS-level failures.

### `Database`

| Method | Description |
|---|---|
| `push(queue, data)` | Enqueue `data` (bytes-like) onto `queue` (str). Durable before returning. |
| `pop(queue) → Message` | Dequeue the oldest message. Raises `QDBEmptyError` if none is available. |
| `ack(msg)` | Permanently remove the message. |
| `nack(msg)` | Return the message to the tail of its queue for retry. `msg.retry_count` increments. |
| `close()` | Close the database. Safe to call more than once. |
| `__enter__` / `__exit__` | Context manager; calls `close()` on exit. |

Calling any method after `close()` raises `ValueError`.

### `Message` (read-only)

| Attribute | Type | Description |
|---|---|---|
| `id` | `int` | Monotonically increasing message identifier. |
| `lease_id` | `int` | Lease identifier; required internally by `ack`/`nack`. |
| `queue` | `str` | Queue name the message was popped from. |
| `data` | `bytes` | Raw payload. |
| `retry_count` | `int` | Number of times this message has been nacked. |

## Exception hierarchy

```
Exception
└── QDBError            # base; catch this for all QDB failures
    ├── QDBIOError      # OS-level I/O failure
    ├── QDBCorruptError # file header or record checksum failure
    ├── QDBEmptyError   # pop on an empty queue
    ├── QDBNotFoundError# message not found (stale ack/nack)
    └── QDBLockedError  # file is already held by another handle
```

`MemoryError` (built-in) is raised on allocation failure.

## Payload types

Only **bytes-like** objects are accepted for `data`: `bytes`, `bytearray`,
`memoryview`.  Passing `str` raises `TypeError`.  Encode strings explicitly:

```python
db.push("q", text.encode())
data = msg.data.decode()
```

## Empty-queue behavior

`pop()` raises `QDBEmptyError` rather than returning `None`.  
This makes the empty case explicit and prevents silent data loss when the
caller forgets to check the return value.

## Context manager recommendation

Always use `Database` as a context manager, or call `close()` explicitly.
Letting the object be garbage collected without closing it emits a
`ResourceWarning` and closes the underlying file handle, but doing so
implicitly risks delayed unlocking and missed close errors.

```python
# Preferred
with qdb.open("work.qdb") as db:
    ...

# Also acceptable
db = qdb.open("work.qdb")
try:
    ...
finally:
    db.close()
```

## Thread safety

`Database` handles are **not thread-safe**.  Use one handle per thread, or
protect a shared handle with `threading.Lock`.  The GIL is released around
blocking C calls, so other Python threads can run while disk I/O is in
progress.

## Running tests

```sh
pip install pytest
pytest python/tests/ -v
```

## Deferred / not yet implemented

The following features exist in the C API but are not yet exposed in Python:

- `qdb_stats()` / `qdb_queue_stats()` — database and per-queue statistics
- `qdb_queue_list()` — enumerate queue names
- `qdb_compact()` / `qdb_compact_recommended()` — log compaction
- `qdb_pop_any()` — dequeue the globally oldest message across all queues
- `qdb_process_expired_leases()` — explicit lease expiry

Distribution deferred:

- PyPI package / binary wheels
- macOS and Windows CI for the Python extension
- Type stubs (`.pyi`)
