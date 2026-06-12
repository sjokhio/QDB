# QDB API Reference

This document covers every public symbol exported by `include/qdb.h`.

---

## Error codes

All fallible functions return `QDB_OK` (0) on success or a negative constant
on failure.  Pointer-returning functions return `NULL` on failure.

| Constant | Value | Meaning |
|---|---|---|
| `QDB_OK` | 0 | Success |
| `QDB_ERR_IO` | −1 | Unspecified I/O error; inspect `errno` |
| `QDB_ERR_CORRUPT` | −2 | Database file is corrupt or unrecognised |
| `QDB_ERR_INVAL` | −3 | Invalid argument (NULL pointer, empty name, …) |
| `QDB_ERR_EMPTY` | −4 | Queue has no available messages |
| `QDB_ERR_NOENT` | −5 | Message ID not found, not leased, or already ACKed |
| `QDB_ERR_NOMEM` | −6 | Memory allocation failed |
| `QDB_ERR_LOCKED` | −7 | Database is locked by another process |

Use `qdb_errmsg(rc)` to get a human-readable description of any error code.

---

## Constants

| Constant | Value | Meaning |
|---|---|---|
| `QDB_QUEUE_NAME_MAX` | 255 | Max queue name length (bytes, excluding NUL) |
| `QDB_MSG_MAX_LEN` | 67 108 864 | Max message payload size (64 MiB) |
| `QDB_VERSION_MAJOR` | 0 | Major version component |
| `QDB_VERSION_MINOR` | 1 | Minor version component |
| `QDB_VERSION_PATCH` | 0 | Patch version component |
| `QDB_VERSION_NUMBER` | computed | `MAJOR*10000 + MINOR*100 + PATCH` |

---

## Types

### `qdb_t`

Opaque database handle.  Obtain with `qdb_open()`; release with `qdb_close()`.
Do not copy, stack-allocate, or dereference directly.  A single handle must
not be shared between threads without external synchronisation.

### `qdb_msg_t`

```c
typedef struct {
    uint64_t  id;        /* opaque monotonic message ID            */
    uint64_t  lease_id;  /* lease identifier granted by qdb_pop()  */
    char     *queue;     /* heap-allocated, null-terminated name   */
    void     *data;      /* heap-allocated payload copy; NULL when len==0 */
    size_t    len;       /* payload length in bytes                */
} qdb_msg_t;
```

**Ownership rules:**

- After a successful `qdb_pop()`, `out_msg->queue` and `out_msg->data` are
  heap-allocated and owned by the caller.
- They survive any subsequent call on the same `qdb_t` handle.
- The caller must release them with `qdb_msg_free()`.
- A zero-initialised `qdb_msg_t` (`= {0}` or `memset` to 0) is always safe to
  pass to `qdb_msg_free()`.

---

## Lifecycle

### `qdb_open`

```c
qdb_t *qdb_open(const char *path);
```

Open (or create) a queue database at `path`.

- If the file does not exist it is created and initialised.
- If the file exists, it is validated, any incomplete tail records are
  truncated, and the full in-memory queue state is reconstructed from the log.
- If a previous session left the dirty flag set (crash without clean close),
  the log is replayed and state is recovered automatically.

`path` is used to derive two sidecar files: `<path>-wal` (write-ahead log,
future) and `<path>-lock` (exclusive file lock).

**Returns:** pointer to a `qdb_t` handle on success; `NULL` on
`QDB_ERR_IO`, `QDB_ERR_CORRUPT`, `QDB_ERR_NOMEM`, or `QDB_ERR_LOCKED`.

---

### `qdb_close`

```c
void qdb_close(qdb_t *db);
```

Flush, unlock, and free all resources associated with `db`.

After this call `db` is invalid.  Passing `NULL` is a safe no-op.

---

## Queue operations

### `qdb_push`

```c
int qdb_push(qdb_t *db, const char *queue, const void *data, size_t len);
```

Durably append a message to `queue`.

- If `queue` does not exist it is created implicitly.
- The write is crash-safe: if the process is killed after `QDB_OK` returns,
  the message will be present after the next `qdb_open()`.
- `data` may be `NULL` only when `len` is zero.

**Parameters:**
- `db` — open handle; must not be `NULL`.
- `queue` — null-terminated name, 1–`QDB_QUEUE_NAME_MAX` bytes.
- `data` — payload bytes; may be `NULL` if `len == 0`.
- `len` — payload length; must be ≤ `QDB_MSG_MAX_LEN`.

**Returns:** `QDB_OK`, `QDB_ERR_INVAL`, `QDB_ERR_IO`, `QDB_ERR_NOMEM`.

---

### `qdb_pop`

```c
int qdb_pop(qdb_t *db, const char *queue, qdb_msg_t *out_msg);
```

Dequeue the oldest PENDING message from `queue` and grant it a time-bounded
lease.

The message transitions from PENDING to LEASED and will not be returned by
subsequent `qdb_pop()` calls until the lease expires or is resolved with
`qdb_ack()` / `qdb_nack()`.

On success `*out_msg` is populated with heap-allocated copies of the queue name
and payload.  The caller owns these and must release them with `qdb_msg_free()`.

**Call `qdb_process_expired_leases()` before `qdb_pop()` in a worker loop** to
ensure messages with expired leases are returned to PENDING before the pop.

**Parameters:**
- `db` — open handle; must not be `NULL`.
- `queue` — null-terminated name; must not be `NULL` or empty.
- `out_msg` — output parameter; must not be `NULL`.

**Returns:** `QDB_OK`, `QDB_ERR_EMPTY`, `QDB_ERR_INVAL`, `QDB_ERR_IO`,
`QDB_ERR_NOMEM`.

---

### `qdb_ack`

```c
int qdb_ack(qdb_t *db, uint64_t msg_id, uint64_t lease_id);
```

Permanently consume the leased message identified by `msg_id`.

The message is marked ACKED and will not be redelivered.  Both `msg_id` and
`lease_id` must match the `qdb_msg_t` returned by the `qdb_pop()` that granted
the lease.  The `lease_id` guard prevents a stale ACK from consuming a message
that has since expired and been re-leased to a different caller.

**Returns:** `QDB_OK`, `QDB_ERR_INVAL` (NULL db or wrong lease_id),
`QDB_ERR_NOENT` (message not found / not leased / already ACKed),
`QDB_ERR_IO`.

---

### `qdb_nack`

```c
int qdb_nack(qdb_t *db, uint64_t msg_id, uint64_t lease_id);
```

Return the leased message to the tail of its source queue as PENDING.

Use `qdb_nack()` when a consumer encounters a transient error and cannot
process the message right now but wants it to be retried.  The message's
internal `retry_count` is incremented.

**Returns:** `QDB_OK`, `QDB_ERR_INVAL` (NULL db or wrong lease_id),
`QDB_ERR_NOENT` (message not found / not leased / already ACKed),
`QDB_ERR_IO`.

---

### `qdb_process_expired_leases`

```c
int qdb_process_expired_leases(qdb_t *db);
```

Scan all active leases; for each whose deadline has passed, write a durable
`RT_MSG_EXPIRE` record and return the message to the tail of its queue as
PENDING.  The message's `retry_count` is incremented.

QDB has **no background thread**.  The application must call this function
explicitly — at the top of each worker loop iteration, or on a periodic timer —
to ensure expired leases are reclaimed.

**Partial progress:** if a disk write fails mid-scan, leases already expired in
this call retain their new PENDING state.  The failing lease and any leases not
yet visited remain active.

**Returns:** number of leases expired (≥ 0) on success; `QDB_ERR_INVAL` if
`db` is `NULL`; `QDB_ERR_IO` if a durable write fails.

---

### `qdb_msg_free`

```c
void qdb_msg_free(qdb_msg_t *msg);
```

Free the heap-allocated `queue` and `data` fields of `*msg` and zero the
struct.

Calling `qdb_msg_free()` on the same pointer more than once is safe.  Passing
`NULL` is a safe no-op.  Passing a pointer to a zero-initialised `qdb_msg_t`
is safe.

---

## Utilities

### `qdb_errmsg`

```c
const char *qdb_errmsg(int err);
```

Return a human-readable, null-terminated description of `err`.  The returned
pointer is a string literal; do not free it.  Unknown codes return
`"unknown error"`.

---

### `qdb_version`

```c
const char *qdb_version(void);
```

Return the library version as a null-terminated string such as `"0.1.0"`.
The pointer is a string literal; do not free it.

---

## Typical worker loop

```c
/* At the top of each iteration, reclaim messages with expired leases. */
qdb_process_expired_leases(db);

qdb_msg_t msg = {0};
int rc = qdb_pop(db, "jobs", &msg);

if (rc == QDB_ERR_EMPTY) {
    /* Nothing to do — sleep or wait for new work. */
} else if (rc == QDB_OK) {
    if (process(msg.data, msg.len) == 0) {
        qdb_ack(db, msg.id, msg.lease_id);   /* success: remove permanently */
    } else {
        qdb_nack(db, msg.id, msg.lease_id);  /* failure: return to tail     */
    }
    qdb_msg_free(&msg);
} else {
    fprintf(stderr, "qdb_pop: %s\n", qdb_errmsg(rc));
}
```
