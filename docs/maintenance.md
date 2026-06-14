# QDB Maintenance Guide

This document covers the operational side of running QDB in production: when
and how to compact, how to back up and restore a database, what to monitor, how
to handle retries, and a practical startup/shutdown/periodic checklist.

---

## Contents

- [When to compact](#when-to-compact)
- [Backup and restore](#backup-and-restore)
- [Monitoring](#monitoring)
- [Retry handling](#retry-handling)
- [Operational checklist](#operational-checklist)

---

## When to compact

QDB's append-only log never reclaims space in place.  Every push, pop, ack,
and nack appends a new record.  `qdb_compact()` is the only way to shrink the
file: it rewrites the database to contain only live messages (PENDING and
LEASED), discarding all ACKED records accumulated since the last compaction.

### What compaction costs

Each compaction writes roughly `3 × (pending + leased)` fsync calls to produce
the staging file, plus one for the atomic rename.  On a macOS NVMe drive this
is around 12 ms per live message at the baseline throughput.  On a lightly
loaded database with a few hundred live messages, compaction takes well under a
second.

### The right signal: `qdb_compact_recommended()`

Use `qdb_compact_recommended()` to ask the library whether compaction is
worthwhile.  It encodes the standard heuristic and requires no arithmetic on
your side:

```c
int recommended = 0;
if (qdb_compact_recommended(db, &recommended) == QDB_OK && recommended) {
    qdb_process_expired_leases(db);   /* expire stale leases first */
    qdb_compact(db);
}
```

The function sets `recommended` to `1` when acked records outnumber live
records (pending + leased), meaning more than half the file is reclaimable
waste.  It sets `recommended` to `0` when there is nothing meaningful to
reclaim (including immediately after a successful compaction, when
`acked_count` resets to zero).

No disk I/O is performed and no memory is allocated.

### Custom thresholds

If the default heuristic does not fit your workload, call `qdb_stats()`
directly and apply your own condition:

```c
qdb_stats_t st = {0};
qdb_stats(db, &st);

/* Example: compact when acked waste exceeds the live working set. */
if (st.acked_count > st.pending_count + st.leased_count) {
    qdb_process_expired_leases(db);
    qdb_compact(db);
}
```

Common reasons to prefer `qdb_stats()` directly:

- Storage-budget trigger: `st.file_size_bytes > N`
- Tighter ratio: compact only when `acked_count > 2 × live`
- You want to log the individual counters alongside the compaction decision

### File-size guidance

`file_size_bytes` gives the absolute on-disk size.  Compare it against your
available disk headroom.  During compaction, QDB writes a staging file at
`<path>-compact` before the rename, so the peak disk usage is approximately
`2 × file_size_bytes`.  Ensure that headroom exists before triggering
compaction on very large databases.

### Frequency

Compact too rarely and the file grows without bound.  Compact too often and
you pay unnecessary fsync cost.  Good trigger conditions:

| Trigger | When it fits |
|---|---|
| `acked_count / (pending + leased) > 1` | General purpose |
| `file_size_bytes > N MB` | Storage-constrained environments |
| On a fixed schedule (e.g. daily at low-traffic hours) | Predictable workloads |
| After every `K` acknowledged messages | High-throughput pipelines |

Do not compact while leases are expected to be active and the consumer is still
processing — compaction preserves LEASED messages, but calling
`qdb_process_expired_leases()` immediately before compact avoids writing
already-expired leases into the compacted file.

### After compaction

Queue entries that have no remaining PENDING or LEASED messages are still
present in the in-memory state after compaction.  They are removed from the
in-memory hash table only after `qdb_close()` followed by `qdb_open()` on the
freshly compacted file, because the compacted file does not write PUSH records
for empty queues.  `qdb_queue_list()` called on the same handle immediately
after compaction may still report queues whose messages were all acked.

---

## Backup and restore

### The database is a single file

QDB's entire state — queue names, message payloads, delivery counts, lease
state — lives in the `.qdb` file.  There are two sidecar files created at
runtime:

| File | Purpose | Back up? |
|---|---|---|
| `<path>.qdb` | Main database | **Yes** |
| `<path>-lock` | Exclusive process lock | **No** — ephemeral; never copy |
| `<path>-wal` | Write-ahead log (future) | Yes, if present |
| `<path>-compact` | Interrupted compaction staging | **No** — QDB deletes it on next open |

The `-lock` file must never be included in a backup or restore; it records the
PID of the owning process and is meaningless on any other system or after a
restart.

### Safe backup procedure

**Method 1: close-copy-reopen (recommended)**

```
1. qdb_close(db)
2. Copy <path>.qdb to your backup location.
3. db = qdb_open(path)  /* re-acquire the lock */
```

This is the only method that guarantees a consistent byte-for-byte snapshot.
It requires a brief window where the database is closed.  On most workloads
the window is under a millisecond.

**Method 2: filesystem snapshot**

If your filesystem supports atomic snapshots (LVM snapshot, APFS snapshot,
ZFS clone), create a snapshot while the database is open.  QDB writes
atomically via two-phase fsync, so a snapshot taken at any point will be
either fully consistent or recoverable (the next open will replay the log and
truncate any partial tail write).

Do not use `cp` on an open database without a snapshot mechanism.  A file
copied byte-by-byte while a write is in progress may capture a split state
where the record header is present but the payload or commit marker is
absent.  QDB's tail-truncation recovery handles this correctly on open, but
you will lose the in-flight message.

**Method 3: periodic checkpoint to a secondary database**

For online backup, periodically pop all PENDING messages, push them to a
backup database, and ack the originals.  This is application-level replication
rather than a file copy and is only appropriate when messages are idempotent.

### Restore procedure

```
1. Ensure no process has the database open (remove the -lock file if
   the owning process is no longer running).
2. Copy the backup .qdb file to the target path.
3. db = qdb_open(path)
```

`qdb_open()` replays the log, validates all records, and reconstructs in-memory
state automatically.  A backup taken while messages were LEASED will restore
with those messages in LEASED state.  Call `qdb_process_expired_leases()`
immediately after open to return any stale leases to PENDING.

### What you cannot restore from

- A corrupt file (bad CRC-32 in any committed record): `qdb_open()` returns
  `NULL` with `QDB_ERR_CORRUPT`.  Only a clean backup can recover this.
- A lost file: there is no WAL at a separate location and no off-host
  replication in v1.

---

## Monitoring

All counters except `file_size_bytes` are derived from in-memory state and
involve no disk I/O.  Poll them freely.

```c
qdb_stats_t st = {0};
qdb_stats(db, &st);
```

### `queue_count`

The number of distinct named queues that have ever been pushed to in this
database session.  Queue entries are created on first push and never removed
within a single open session, even after all messages on a queue are
acknowledged.  This counter reflects total capacity usage, not current
activity.

**Alert on:** unexpected growth (queues being created with dynamic or
user-supplied names that are not validated upstream).

### `pending_count`

Messages ready to be popped across all queues.  This is the primary
consumer health metric.

- A value of 0 means the queue is drained.
- A value that grows monotonically means consumers are not keeping up with
  producers.  Check consumer health, lease timeout configuration, and whether
  `qdb_process_expired_leases()` is being called.

**Alert on:** sustained growth over time, or a value above your expected
working set.

### `leased_count`

Messages currently held by an active lease.  In a healthy system this should
be approximately equal to the number of active consumer goroutines/threads
(one message in flight per consumer at a time, given the single-threaded
handle model).

- A value much higher than the consumer count suggests consumers are crashing
  or hanging without calling `qdb_ack()` or `qdb_nack()`.
- After a crash-and-restart, the count reflects leases that were active at
  crash time.  Call `qdb_process_expired_leases()` to resolve them.

**Alert on:** a value that grows after startup without a matching increase in
consumer count.

### `acked_count`

Messages acknowledged since the last compaction (or since open if never
compacted).  This is proportional to the amount of reclaimable space in the
log file.  It should rise during normal operation and drop to 0 or near-0
after each successful compaction.

**Alert on:** a value that keeps growing without a corresponding compaction;
cross-check with `file_size_bytes` to confirm disk usage is also growing.

### `file_size_bytes`

The raw size of the `.qdb` file.  Use this to monitor disk space consumption
and to estimate the benefit of the next compaction.

Approximate reclaimable space:

```
reclaimable ≈ file_size_bytes × (acked_count / (pending + leased + acked))
```

This is an estimate because record sizes vary with payload length.

**Alert on:** `file_size_bytes` approaching the available disk headroom, or
growing faster than expected given `acked_count`.

### Per-queue monitoring

Use `qdb_queue_list()` to enumerate queues, then `qdb_queue_stats()` on each
to get per-queue `pending_count`, `leased_count`, and `acked_count`:

```c
qdb_queue_name_t names[64];
size_t           count;
qdb_queue_list(db, names, 64, &count);

size_t i, limit = count < 64 ? count : 64;
for (i = 0; i < limit; i++) {
    qdb_queue_stats_t qs = {0};
    qdb_queue_stats(db, names[i].name, &qs);
    /* log / export qs.pending_count, qs.leased_count, qs.acked_count */
}
```

---

## Retry handling

### The `retry_count` field

Every `qdb_msg_t` returned by `qdb_pop()` includes `retry_count`: the number
of times this message has been returned to the queue without being
acknowledged.  It is 0 on the first delivery.

`retry_count` is incremented by:
- `qdb_nack()` — consumer explicitly returned the message.
- `qdb_process_expired_leases()` — the consumer's lease expired without a
  resolution.

The count survives crashes and close/reopen.  A message nacked twice before a
crash will have `retry_count == 2` after the database is reopened.

### Dead-letter threshold pattern

The most common pattern: ack and route the message elsewhere once it has
failed too many times.

```c
#define MAX_RETRIES 5

qdb_msg_t msg = {0};
if (qdb_pop(db, "jobs", &msg) == QDB_OK) {

    if (msg.retry_count >= MAX_RETRIES) {
        /* Too many failures — move to dead-letter queue or discard. */
        push_to_dead_letter(db, &msg);           /* application-defined */
        qdb_ack(db, msg.id, msg.lease_id);       /* remove from jobs queue */
    } else if (process(msg.data, msg.len) == SUCCESS) {
        qdb_ack(db, msg.id, msg.lease_id);
    } else {
        qdb_nack(db, msg.id, msg.lease_id);      /* increment retry_count */
    }

    qdb_msg_free(&msg);
}
```

### Separate dead-letter queue

A common pattern is a second QDB queue (in the same or a different database)
for messages that have exceeded the retry threshold:

```c
void push_to_dead_letter(qdb_t *db, const qdb_msg_t *msg)
{
    qdb_push(db, "jobs.dead", msg->data, msg->len);
}
```

The dead-letter queue can be processed separately — alerting an operator,
writing to a log file, or triggering a manual review workflow.

### Backoff pattern

If a consumer wants to delay retry rather than fail immediately, push to a
"retry later" queue and let a background worker re-enqueue them after a
delay.  This is application-level logic; QDB does not have a built-in
delay-queue or visibility timeout beyond the lease window.

```c
/* On failure: send to the delay queue instead of nacking. */
char retry_queue[32];
snprintf(retry_queue, sizeof(retry_queue), "jobs.retry.%u",
         msg.retry_count);         /* separate queue per attempt number */
qdb_push(db, retry_queue, msg.data, msg.len);
qdb_ack(db, msg.id, msg.lease_id);
```

### Poison message detection

If `pending_count` is non-zero but every pop yields a message with a very
high `retry_count`, the consumer logic itself may be broken for the current
message class.  Consider alerting when `retry_count` exceeds a secondary
threshold (e.g. 3× `MAX_RETRIES`) before moving to the dead-letter queue,
so a systematic bug does not silently drain the dead-letter queue faster than
it can be reviewed.

### Application ownership

QDB provides the counter and the mechanism; all retry policies are
application-defined.  There is no built-in rate limiting, delay, or
escalation.  This is intentional: retry semantics vary too widely between
applications to provide a useful built-in default.

---

## Operational checklist

### Startup

```
☐ Call qdb_open() (or qdb_open_ex() with your lease timeout).
  On crash recovery, qdb_open() replays the log automatically.

☐ Call qdb_process_expired_leases() before the first qdb_pop().
  After a crash, messages that were LEASED at crash time are restored in
  LEASED state.  Calling qdb_process_expired_leases() once the lease
  deadline has passed returns them to PENDING for redelivery.
  (On a clean shutdown the lease timeout period must still elapse; if
   your lease_timeout_s is 30, wait at least 30 seconds or use a short
   timeout for restart scenarios.)
```

### Worker loop

```c
for (;;) {
    /* Always call this at the top of the loop — not just on startup. */
    qdb_process_expired_leases(db);

    qdb_msg_t msg = {0};
    int rc = qdb_pop(db, "jobs", &msg);

    if (rc == QDB_ERR_EMPTY) {
        sleep_or_wait();
        continue;
    }
    if (rc != QDB_OK) {
        handle_error(rc);
        continue;
    }

    if (msg.retry_count >= MAX_RETRIES) {
        push_to_dead_letter(db, &msg);
        qdb_ack(db, msg.id, msg.lease_id);
    } else if (process(msg.data, msg.len) == 0) {
        qdb_ack(db, msg.id, msg.lease_id);
    } else {
        qdb_nack(db, msg.id, msg.lease_id);
    }

    qdb_msg_free(&msg);
}
```

### Shutdown

```
☐ Call qdb_close(db).
  This writes the final header (clears the dirty flag), releases the file
  lock, and frees all in-memory state.  No explicit flush is needed; the
  database is already fsync'd after each operation.

☐ Do not delete the .qdb file on shutdown.
  The file is the persistent state. Delete it only if you intend to discard
  the queue permanently.

☐ Do not delete the -lock file manually while the process is running.
  Another process could then open the database concurrently, leading to
  corruption.
```

### Periodic maintenance

```
☐ Compact when acked_count significantly exceeds the live message count,
  or when file_size_bytes approaches your storage threshold.
  Always call qdb_process_expired_leases() immediately before qdb_compact().

☐ Monitor pending_count for consumer backlog.

☐ Monitor leased_count for stuck or crashed consumers.

☐ Monitor acked_count and file_size_bytes to schedule the next compaction.

☐ Take backups by closing the database, copying the .qdb file, and reopening.
  Do not include the -lock file in the backup.
```

---

*See also:*
- [`docs/reliability.md`](reliability.md) — durability guarantees, lease mechanics, crash recovery protocol
- [`docs/api.md`](api.md) — complete public API reference
- [`docs/benchmarks.md`](benchmarks.md) — performance baseline and fsync cost analysis
