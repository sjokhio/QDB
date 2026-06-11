# QDB Queue Semantics

This document defines the observable behaviour of QDB queues from the application's perspective: ordering, delivery guarantees, naming rules, lease behaviour, and the rules governing interaction between multiple queues in a single database.

---

## 1. Queue Model

A QDB **queue** is:

- A **named** sequence of messages.
- **FIFO**: messages are returned in the order they were successfully pushed.
- **Persistent**: messages survive process restarts and crashes.
- **Isolated**: queues within the same database do not interact.

### 1.1 Implicit creation

Queues do not require explicit creation.  The first `qdb_push()` to a named queue creates it implicitly.  There is no `qdb_create_queue()` call.

**Rationale:**  Requiring explicit creation adds ceremony without benefit.  SQLite does not require `CREATE DATABASE` before `CREATE TABLE`.  QDB does not require an explicit queue creation step.

### 1.2 Implicit deletion

There is no `qdb_delete_queue()` in v1.  A queue that becomes empty (all messages acked) simply has no messages.  It consumes no meaningful resources in either the in-memory index (an empty `QueueState` is a few bytes) or on disk (the queue name appears only in `RT_MSG_PUSH` records, all of which are dead after acking and removed at compaction).

---

## 2. FIFO Ordering Guarantee

**Rule:**  If `qdb_push(db, "q", A)` returns `QDB_OK` before `qdb_push(db, "q", B)` begins, then `qdb_pop(db, "q")` will return A before B on any subsequent call.

This is a strict FIFO guarantee on first delivery.

### 2.1 Ordering after redelivery

Messages that are redelivered (after lease expiry or NACK) are returned to the **front** of the queue, not the back.  This means a redelivered message may precede messages that were pushed after it.

**Example:**

```
Time 0: push A  → queue: [A]
Time 1: push B  → queue: [A, B]
Time 2: pop     → returns A; queue: [B], pending: {A}
Time 3: lease expires → queue: [A, B], pending: {}
Time 4: pop     → returns A again (redelivery)
```

After the expiry at time 3, A is at the front despite B having been in the queue longer in wall-clock terms.

**Rationale:**  A redelivered message represents work that was attempted but not confirmed.  It is more urgent than fresh work.  Placing it at the front minimises the retry latency.  Applications that need strict FIFO even across redeliveries must implement their own sequencing using the message ID.

### 2.2 Ordering across different queues

There is no ordering guarantee across different queues.  If `push("q1", A)` happens before `push("q2", B)`, there is no guarantee about the relative order in which A and B are returned by pops on their respective queues.

---

## 3. Delivery Semantics

### 3.1 At-least-once delivery

QDB provides **at-least-once delivery**: every pushed message is delivered to a consumer at least once.

A message is permanently removed only when explicitly acknowledged with `qdb_ack()`.  Until that point, the message will eventually be redelivered (either immediately, or after the lease expires following a crash or timeout).

### 3.2 No exactly-once delivery

QDB does not provide exactly-once delivery.  Duplicate delivery can occur when:

1. A consumer pops message A, processes it, and crashes before calling `qdb_ack()`.
2. The lease expires.
3. A consumer (possibly a new instance of the same process) pops message A again.

Consumers must be prepared for duplicates.  The canonical approach is to make consumer processing **idempotent**: processing a message twice has the same effect as processing it once.  The `msg_id` field provides a stable identifier that consumers can use to deduplicate.

**Alternative considered — two-phase commit:**  The consumer participates in a distributed transaction; the ack and the downstream effect are committed atomically.  This requires the consumer's data store and QDB to participate in 2PC, which is complex, slow, and impractical for most deployments.  Rejected for v1.

### 3.3 No message loss on push

A message for which `qdb_push()` returns `QDB_OK` will not be lost.  It will be delivered at least once regardless of what happens after the push.

A message for which `qdb_push()` returns an error code has not been committed and may or may not exist in the queue — the caller must treat it as not pushed and retry if needed.

---

## 4. Lease Semantics

### 4.1 What a lease is

A **lease** is a time-bounded exclusive claim on a message.  When `qdb_pop()` succeeds:

1. The message transitions from READY to LEASED.
2. The message is invisible to other `qdb_pop()` calls until the lease expires or is resolved.
3. The consumer holds the `msg_id` and must call `qdb_ack()` or `qdb_nack()` before the lease expires.

### 4.2 Lease duration

The default lease duration is **30 seconds**.  This is a per-database configuration at open time (future API: `qdb_options_t`).

The lease duration should be set to comfortably exceed the expected processing time for a single message, with margin for outlier latencies.  Too short a lease causes spurious redeliveries; too long a lease means a crashed consumer holds a message for a long time before it is redelivered.

### 4.3 Lease expiry behaviour

When a lease expires:

1. The message is written back to the **front** of its queue as an `RT_MSG_EXPIRE` record.
2. The in-memory index moves it from `pending_acks` to the front of `ready_msgs`.
3. The next `qdb_pop()` on this queue will return the message.

Lease expiry is **passive** — it is evaluated on the next `qdb_pop()` call (or by a background timer in a future version).  Leases do not expire automatically in real time without a trigger.

**Implication:**  If `qdb_pop()` is not called frequently, a message may remain in LEASED state beyond its nominal expiry without being immediately redelivered.  For v1, this is acceptable.

### 4.4 Stale ACKs

A stale ACK occurs when a consumer calls `qdb_ack()` with a `msg_id` after the lease for that message has expired and the message has been re-leased to another consumer.

QDB detects stale ACKs via the `lease_id` field:
- Each `RT_MSG_LEASE` record carries a unique, monotonically increasing `lease_id`.
- The consumer does not see the `lease_id` directly (it is internal).
- `qdb_ack()` looks up the `msg_id` in `pending_acks`; if the message is not there (it was re-leased and the old `pending_acks` entry was overwritten), or if the `lease_id` doesn't match, `QDB_ERR_NOENT` is returned.

**Example:**

```
Consumer 1 pops msg 42 (lease_id=7, expiry=T+30s)
Consumer 1 takes 60 seconds (lease expires)
Lease expiry fires: msg 42 returns to queue
Consumer 2 pops msg 42 (lease_id=8, expiry=T+90s)
Consumer 2 acks msg 42 (lease_id=8) → QDB_OK  (permanent deletion)
Consumer 1 tries to ack msg 42 (lease_id=7) → QDB_ERR_NOENT (stale, ignored)
```

---

## 5. Queue Naming Rules

Queue names follow these constraints:

| Rule | Constraint |
|---|---|
| Encoding | Valid UTF-8 |
| Length | 1–255 bytes (byte length, not character count) |
| Allowed characters | Any valid UTF-8 sequence except null byte (`\0`) |
| Case sensitivity | Names are case-sensitive: `"Jobs"` and `"jobs"` are different queues |
| Leading/trailing whitespace | Permitted but strongly discouraged |

**Recommended naming conventions (not enforced):**
- Use lowercase ASCII with hyphens: `email-delivery`, `payment-webhooks`
- Use slashes for namespacing: `service-a/jobs`, `service-b/jobs`
- Avoid control characters and Unicode whitespace

**Rationale for permitting all non-null UTF-8:**  Restricting to ASCII is simpler to implement but unnecessarily limits multilingual deployments.  The null-byte exclusion is required because queue names are stored with a length prefix and compared with `memcmp`, but the exclusion also protects against C string confusion bugs.

---

## 6. Multiple Queues in One Database

A single `qdb_t` handle provides access to all queues in the database.  There is no concept of "switching" to a queue; the queue name is passed on every operation.

### 6.1 Isolation

Operations on queue `A` are completely independent from operations on queue `B`:
- Pushing to `A` does not affect `B`.
- Popping from `A` does not affect `B`.
- A message in LEASED state on `A` does not block pops on `B`.

### 6.2 Shared resources

All queues in a database share:
- The append-only log (all records go to the same file)
- The WAL (a single WAL file covers all queues)
- The file lock
- The monotonic message ID counter (`next_msg_id`)

The shared log means that a very high write rate to queue A will interleave its records with records from queue B in the log.  This is correct and expected.

### 6.3 No cross-queue transactions

In v1, there are no transactions that span multiple queues.  Atomically moving a message from queue A to queue B is not supported.  This is a deliberate scope limitation.

---

## 7. Empty Queue Behaviour

Calling `qdb_pop()` on an empty queue returns `QDB_ERR_EMPTY` immediately.  QDB does not block waiting for a message to become available (no blocking pop / long-polling).

**Rationale:**  A blocking pop requires either a background thread or a callback mechanism.  Both add API and implementation complexity.  A non-blocking pop with caller-side polling or a `select()`/`epoll()`-based wait is the composable, UNIX-philosophy alternative.  A future `qdb_fd()` API that returns a pollable file descriptor (similar to SQLite's `sqlite3_file_control`) could provide efficient waiting without blocking the API.

---

## 8. Message Size Constraints

| Constraint | Value | Rationale |
|---|---|---|
| Minimum payload size | 1 byte | Zero-length messages carry no information |
| Maximum payload size | 64 MiB | Prevents runaway memory allocation in the read buffer; matches common embedded queue size limits |
| Maximum queue name length | 255 bytes | Fits in a `uint8_t` length prefix; consistent with DNS label length limits |

Large messages (approaching 64 MiB) are supported but not encouraged.  QDB is designed for job descriptors, event records, and small payloads — not for bulk data transfer.  For large payloads, store the data externally and push a reference (URL, file path, blob ID) into the queue.

---

## 9. Observable Properties Summary

| Property | Value |
|---|---|
| Delivery guarantee | At-least-once |
| Ordering | FIFO within a queue on first delivery; redeliveries go to front |
| Persistence | Crash-safe; survives process and system crashes |
| Isolation | Full isolation between queues |
| Message ID | Monotonic `uint64_t`, unique per database |
| Lease model | Time-bounded exclusive claim; stale ACKs are rejected |
| Blocking pop | Not supported (v1); returns `QDB_ERR_EMPTY` immediately |
| Cross-queue transactions | Not supported (v1) |
| Multi-process access | Not supported (v1); enforced via lock file |
| Multi-thread access | Not supported (v1); caller must serialize |
