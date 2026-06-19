# QDB Status

This document describes the current state of QDB, what is intentionally
absent, and the reliability guarantees that already hold.

---

## What works

### Storage engine
- Single append-only log file per database.
- 4 096-byte file header with magic bytes, format version, and CRC-32 field.
- Variable-length records with payload CRC-32 and a two-phase commit marker
  (`0xAB`) to distinguish fully-written records from partial tail writes.
- On open, partial tail records are detected and truncated automatically.
- Exclusive file-lock prevents two processes from opening the same database.

### Message lifecycle

| API | Status |
|---|---|
| `qdb_open` / `qdb_close` | Complete |
| `qdb_open_ex` (configurable lease timeout) | Complete |
| `qdb_push` | Complete |
| `qdb_pop` (with lease) | Complete |
| `qdb_pop_any` (global oldest across all queues) | Complete |
| `qdb_ack` | Complete |
| `qdb_nack` | Complete |
| `qdb_process_expired_leases` | Complete |
| `qdb_compact` | Complete |
| `qdb_stats` / `qdb_queue_stats` | Complete |
| `qdb_compact_recommended` | Complete |

### Crash recovery
- The log is replayed in full on every open.
- All five record types (PUSH, LEASE, ACK, NACK, EXPIRE) are replayed and
  validated.
- Consistency checks: duplicate message IDs, ACK/NACK of unknown or
  wrong-lease messages, and leasing an already-ACKed message all return
  `QDB_ERR_CORRUPT` on open, refusing to operate on a damaged database.
- `retry_count` is reconstructed from the replay; messages that were
  NACK'd or expired before a crash come back with the correct count.
- Multi-process crash recovery verified: worker processes are killed
  mid-operation and the parent process confirms correct state after reopen.

### Compaction
- `qdb_compact()` rewrites the database to a staging file containing only
  live queue state (PENDING and LEASED messages), then atomically renames it
  over the original.
- Crash-safe: a crash before the rename leaves the original database intact;
  a crash after the rename leaves the compacted database.
- Stale `-compact` sidecar files left by interrupted compactions are cleaned
  up automatically on the next `qdb_open()`.
- Cross-platform: POSIX uses `rename()`; Windows closes the handle before
  `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`.

### Observability
- `qdb_stats()` returns database-level counts (pending, leased, acked,
  queue count, file size) without disk I/O.
- `qdb_queue_stats()` returns per-queue counts.

### In-memory state
- Separate-chaining hash tables for messages (1 024 buckets), queues
  (64 buckets), and leases (1 024 buckets).
- Intrusive doubly-linked FIFO list per queue for O(1) head/tail access.
- Fibonacci hashing for integer keys; FNV-1a for queue name keys.

### Test coverage
- 16 test suites.
- Storage layer, log replay, push, pop, ack, nack, lease expiry, stats,
  open_ex, multi-process behaviour, and compaction each have a dedicated suite.
- I/O failure simulation (close fd mid-operation) verifies that no
  in-memory mutation occurs before the disk write succeeds.
- Multi-process crash recovery tests cover acked-then-crash and nacked-then-crash
  scenarios via a worker process that is killed at the critical moment.
- Three fuzz harnesses (header, record parser, full replay) run in CI as
  30-second smoke tests and as standalone corpus-replay tools.

---

## What is intentionally missing

### Dead-letter queue / retry limit
`retry_count` is tracked per message, survives crashes and close/reopen, and
is exposed in `qdb_msg_t` (populated by `qdb_pop()`).  There is no built-in
policy to move messages to a dead-letter queue after N failures; callers check
`msg.retry_count` after each `qdb_pop()` and implement their own threshold.

### Batch push / batch pop
Every push and pop involves at least 2 and 4 `fsync` calls respectively.
Batching multiple messages per `fsync` (group commit) is the primary lever
for throughput improvement and is not yet implemented.

### Background lease expiry
Lease expiry is purely caller-driven (`qdb_process_expired_leases`).  There
is no background thread.  Applications that need prompt expiry must call the
function periodically.

### WAL / checkpoint path
The code has a `replay_wal` function and WAL-related header fields that were
scaffolded early, but the WAL write path is not wired to push/pop/ack today.
All writes go directly to the main log file.  The WAL machinery is reserved
for a future group-commit implementation.

---

## Reliability guarantees

### Durability
A call that returns `QDB_OK` is durable.  The record is in the file and has
been fsync'd to storage (or `F_FULLFSYNC` on macOS).  An immediate process
kill after `QDB_OK` will not lose the message.

### At-least-once delivery
A message returned by `qdb_pop` will be redelivered if the process exits
before calling `qdb_ack`.  On the next `qdb_open`, the lease record is
replayed and the message is left in LEASED state.  Calling
`qdb_process_expired_leases` after open (once the lease deadline passes)
returns it to PENDING.

### No silent data loss
Every record has a CRC-32.  A bit flip in a committed record is detected on
the next open and the database is refused with `QDB_ERR_CORRUPT`.  The library
never silently drops messages.

### Ordering within a queue
Messages within a single queue are always delivered in FIFO order for PENDING
messages.  NACKed or expired messages are appended to the tail; they do not
jump the queue.

### Isolation between queues
Queues are completely independent.  A slow or failed consumer on one queue has
no effect on other queues in the same database.

---

## Non-goals for v1

- Multi-process or networked access to the same database file.
- Pub/sub, fan-out, or topic routing.
- Throughput above ~5 000 msg/s (fsync cost dominates at single-message
  granularity; group commit will raise this substantially).
- Encryption at rest.
- Authentication or access control.
- Distributed consensus or replication.
