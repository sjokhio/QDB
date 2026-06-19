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
| `QDB_VERSION_MAJOR` | 1 | Major version component |
| `QDB_VERSION_MINOR` | 0 | Minor version component |
| `QDB_VERSION_PATCH` | 0 | Patch version component |
| `QDB_VERSION_NUMBER` | computed | `MAJOR*10000 + MINOR*100 + PATCH` |

---

## Types

### `qdb_t`

Opaque database handle.  Obtain with `qdb_open()`; release with `qdb_close()`.
Do not copy, stack-allocate, or dereference directly.  A single handle must
not be shared between threads without external synchronisation.

### `qdb_queue_name_t`

```c
typedef struct {
    char name[QDB_QUEUE_NAME_MAX + 1];
} qdb_queue_name_t;
```

Single queue name entry returned by `qdb_queue_list()`.  `name` is always
null-terminated.  Always zero-initialise before use.

### `qdb_msg_t`

```c
typedef struct {
    uint64_t  id;           /* opaque monotonic message ID                  */
    uint64_t  lease_id;     /* lease identifier granted by qdb_pop()        */
    char     *queue;        /* heap-allocated, null-terminated name          */
    void     *data;         /* heap-allocated payload copy; NULL when len==0 */
    size_t    len;          /* payload length in bytes                       */
    uint32_t  retry_count;  /* delivery attempts after the first (0 = first) */
} qdb_msg_t;
```

**Ownership rules:**

- After a successful `qdb_pop()`, `out_msg->queue` and `out_msg->data` are
  heap-allocated and owned by the caller.
- They survive any subsequent call on the same `qdb_t` handle.
- The caller must release them with `qdb_msg_free()`.
- A zero-initialised `qdb_msg_t` (`= {0}` or `memset` to 0) is always safe to
  pass to `qdb_msg_free()`.

**`retry_count` usage:**

`retry_count` is incremented each time the message is returned to the queue
without being acknowledged — by `qdb_nack()` or by `qdb_process_expired_leases()`
when a lease expires.  It survives crashes and close/reopen.  Use it to
implement a dead-letter threshold:

```c
if (msg.retry_count >= MAX_RETRIES) {
    /* move to dead-letter queue or discard */
    qdb_ack(db, msg.id, msg.lease_id);
} else {
    qdb_nack(db, msg.id, msg.lease_id);   /* retry */
}
```

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

### `qdb_open_ex`

```c
qdb_t *qdb_open_ex(const char *path, const qdb_open_opts_t *opts);
```

Open (or create) a queue database with configuration options.

Identical to `qdb_open()` when `opts` is `NULL` or zero-initialised.
Use this function when you need to configure the lease timeout or any
future open-time option.

**`qdb_open_opts_t`:**

```c
typedef struct {
    uint32_t lease_timeout_s;  /* 0 → QDB_DEFAULT_LEASE_TIMEOUT_S (30 s) */
} qdb_open_opts_t;
```

Always zero-initialise before setting fields so that future fields default
correctly:

```c
qdb_open_opts_t opts = {0};
opts.lease_timeout_s = 120;   /* 2-minute lease window */
qdb_t *db = qdb_open_ex("work.qdb", &opts);
```

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

**OOM-after-durable-write:** If `QDB_ERR_NOMEM` is returned and the record
was already written to disk, the handle's in-memory state is inconsistent.
Call `qdb_close()` and reopen the database before continuing.

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

### `qdb_pop_any`

```c
int qdb_pop_any(qdb_t *db, qdb_msg_t *out_msg);
```

Dequeue the globally oldest PENDING message across **all queues** and grant it
a time-bounded lease.

Equivalent to `qdb_pop()` but without naming a specific queue.  The message
with the lowest message ID across every queue's `pending_head` is selected.
Because message IDs are assigned from a single global monotonic counter at push
time, the minimum `pending_head` ID is the oldest currently-available message
in the entire database.

The source queue name is available in `out_msg->queue` after a successful call.

On success `*out_msg` is populated with heap-allocated copies of the queue name
and payload.  The caller owns these and must release them with `qdb_msg_free()`.

**Call `qdb_process_expired_leases()` before `qdb_pop_any()` in a worker loop**
to ensure messages with expired leases are returned to PENDING before the pop.

**Ordering guarantees:**

- Within a single queue, messages are returned in push order (FIFO).
- Across queues, messages are returned in global push order: the message pushed
  earliest (lowest ID) is returned first, regardless of which queue it belongs
  to.
- A NACK'd or expired message goes to the **tail** of its queue, not back to the
  position it was originally pushed from.  Its ID does not change, so it will
  appear after any message that was pushed after it but not yet consumed.

**Parameters:**
- `db` — open handle; must not be `NULL`.
- `out_msg` — output parameter; must not be `NULL`.

**Returns:** `QDB_OK`, `QDB_ERR_EMPTY` (no pending messages anywhere),
`QDB_ERR_INVAL`, `QDB_ERR_IO`, `QDB_ERR_NOMEM`.

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

Return the library version as a null-terminated string such as `"1.0.0"`.
The pointer is a string literal; do not free it.

---

## Observability

### `qdb_stats`

```c
int qdb_stats(qdb_t *db, qdb_stats_t *out);
```

Fill `*out` with database-level statistics.  All counts are derived from
in-memory state — no disk I/O is performed.

**`qdb_stats_t`:**

```c
typedef struct {
    uint64_t queue_count;    /* number of distinct named queues              */
    uint64_t pending_count;  /* messages in PENDING state across all queues  */
    uint64_t leased_count;   /* messages in LEASED state across all queues   */
    uint64_t acked_count;    /* messages ACKed since last compaction          */
    uint64_t file_size_bytes; /* current log file size in bytes              */
} qdb_stats_t;
```

**Returns:** `QDB_OK`, `QDB_ERR_INVAL` (NULL argument), `QDB_ERR_IO`
(file-size query failed).

---

### `qdb_queue_stats`

```c
int qdb_queue_stats(qdb_t *db, const char *queue, qdb_queue_stats_t *out);
```

Fill `*out` with per-queue statistics for `queue`.

**`qdb_queue_stats_t`:**

```c
typedef struct {
    uint64_t pending_count;  /* messages in PENDING state in this queue */
    uint64_t leased_count;   /* messages in LEASED state in this queue  */
    uint64_t acked_count;    /* messages ACKed in this queue             */
} qdb_queue_stats_t;
```

**Returns:** `QDB_OK`, `QDB_ERR_INVAL` (NULL argument or empty name),
`QDB_ERR_NOENT` (queue does not exist).

---

### `qdb_queue_list`

```c
int qdb_queue_list(qdb_t *db,
                   qdb_queue_name_t *out, size_t cap,
                   size_t *out_count);
```

Enumerate queue names into a caller-provided buffer.

Copies up to `cap` queue names into `out[]`.  `*out_count` is always set to
the total number of queues in the database, even when that exceeds `cap`.
When `*out_count > cap` the buffer was too small; allocate a larger buffer and
call again.

To query the count without copying names, pass `out = NULL` and `cap = 0`.

Names are returned in unspecified order.  Sort the result if a stable order
is needed (e.g. for display).

No disk I/O is performed.  No memory is allocated by the library.

Queue entries persist for the lifetime of the handle: a queue created by a
push and later fully acknowledged still appears.  Entries are removed only by
`qdb_compact()` followed by close and reopen.

**Parameters:**
- `db` — open handle; must not be `NULL`.
- `out` — caller-allocated array of at least `cap` entries; may be `NULL`
  when `cap` is `0`.
- `cap` — capacity of `out` in entries.
- `out_count` — output: total queue count; must not be `NULL`.

**Returns:** `QDB_OK`, `QDB_ERR_INVAL` (`db` or `out_count` is NULL, or
`out` is NULL while `cap > 0`).

**Typical usage:**

```c
/* Count-only */
size_t n;
qdb_queue_list(db, NULL, 0, &n);

/* Fixed stack buffer */
qdb_queue_name_t names[64];
size_t count;
if (qdb_queue_list(db, names, 64, &count) == QDB_OK) {
    size_t i, limit = count < 64 ? count : 64;
    for (i = 0; i < limit; i++)
        printf("%s\n", names[i].name);
}

/* Compose with qdb_queue_stats() */
qdb_queue_stats_t qs = {0};
qdb_queue_stats(db, names[0].name, &qs);
```

---

## Maintenance

### `qdb_compact`

```c
int qdb_compact(qdb_t *db);
```

Rewrite the database file to contain only live queue state, discarding
ACKed message records accumulated since the last compaction (or since open).

**What compaction does:**

1. Writes a staging file (`<path>-compact`) containing a CHECKPOINT record
   (to pin `next_msg_id` and `next_lease_id`) followed by PUSH + LEASE
   records for every PENDING and LEASED message in the current database,
   preserving original message IDs, lease IDs, and expiry timestamps.
2. Atomically renames the staging file over the original database file.
3. Reopens the compacted file and rebuilds in-memory state.

**Recommended usage:**

```c
/* Expire stale leases before compacting so they are not written to the
 * compacted file and immediately re-expired on the next open. */
qdb_process_expired_leases(db);
int rc = qdb_compact(db);
if (rc != QDB_OK) { /* see failure contract below */ }
```

**Crash-safety:**

- A crash *before* the rename leaves the original database intact and the
  staging file as a harmless sidecar.  The staging file is deleted the next
  time `qdb_open()` is called on the same path.
- A crash *after* the rename leaves the compacted database, which is
  self-consistent and replayed normally on the next `qdb_open()`.

**Failure contract:**

- Failure *before* the database file is replaced (staging-file errors, flush
  errors, rename errors): the original database is intact and the handle
  remains valid.  You may retry or continue using the handle.
- Failure *after* the database file has been replaced (the internal reopen
  step fails): the handle is explicitly invalidated — `db->fd` is closed and
  internal state is freed.  Do **not** use the handle for any further
  operations; call `qdb_close()` to release remaining resources, then reopen
  the database from disk.

**Returns:** `QDB_OK`, `QDB_ERR_INVAL` (NULL db), `QDB_ERR_IO` (staging
write, fsync, rename, or reopen failed), `QDB_ERR_NOMEM`.

---

### `qdb_compact_recommended`

```c
int qdb_compact_recommended(qdb_t *db, int *out_recommended);
```

Heuristic check for whether calling `qdb_compact()` is likely to reclaim
significant space.  Sets `*out_recommended` to `1` (compact) or `0` (skip).
Does **not** trigger compaction; the caller decides whether to act.

**Heuristic:**

```
recommended = (acked_count > 0) &&
              (acked_count > pending_count + leased_count)
```

Compaction is recommended when there are acknowledged records to reclaim AND
the acked record count exceeds the live record count (pending + leased).
This means reclaimable record slots outnumber live record slots — more than
half the file is recoverable waste.

The ratio is based on **record counts, not byte sizes**.  With variable
payload sizes the true reclaimable fraction may differ.  For
storage-budget–based triggers (e.g. `file_size_bytes > N`), call
`qdb_stats()` directly and apply your own threshold.

No disk I/O is performed beyond what `qdb_stats()` does internally.
No memory is allocated.

**Parameters:**
- `db` — open handle; must not be `NULL`.
- `out_recommended` — output: `1` if compaction is recommended, `0` otherwise;
  must not be `NULL`.

**Returns:** `QDB_OK`, `QDB_ERR_INVAL` (`db` or `out_recommended` is NULL).

**Typical usage:**

```c
int recommended = 0;
if (qdb_compact_recommended(db, &recommended) == QDB_OK && recommended) {
    qdb_process_expired_leases(db);
    qdb_compact(db);
}
```

**When to prefer `qdb_stats()` directly:**

- You want to trigger on `file_size_bytes > N` (storage-budget threshold).
- You want a different ratio (e.g. compact when acked > 2× live).
- You want to log the individual counters alongside the decision.

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
