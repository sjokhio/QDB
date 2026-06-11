# QDB Architecture

This document describes the design goals, philosophy, and planned internal architecture of QDB.  It is a living document; sections marked **[Planned]** describe design intent that has not yet been implemented.

---

## 1. Project Goals

QDB exists to solve one problem well: **durable embedded message queues**.

The target use-cases are applications that need reliable, ordered, persistent queues without the operational overhead of running a separate broker process.  Examples include:

- Job queues in application servers that must survive a crash or restart
- Audit logs that must be consumed in order
- Background task pipelines in embedded or edge systems
- Offline-first applications that queue operations for later sync

QDB is not a distributed system.  It does not replicate data across machines, it does not elect leaders, and it does not expose a network protocol.  A single QDB file is owned by a single process at a time.

---

## 2. Reliability-First Philosophy

Every design decision in QDB is evaluated first on the axis of **correctness under failure**, and second on performance.

This means:

- A message that `qdb_push()` returns `QDB_OK` for **will not be lost**, even if the process is killed immediately after.
- A message dequeued by `qdb_pop()` but not yet acknowledged **will be redelivered** after a crash, providing at-least-once delivery semantics.
- QDB will never silently corrupt a database.  If it cannot safely complete an operation, it returns an error.

The trade-off is deliberate: QDB calls `fsync` (or its platform equivalent) on the critical path.  Raw throughput is secondary to the guarantee that completed operations are durable.

A future `QDB_FLAG_RELAXED_DURABILITY` mode may allow callers to opt out of `fsync` for workloads that can tolerate OS-level durability (data survives process crash but not kernel crash), but the default will always be strict.

---

## 3. Append-Only Storage Design **[Planned]**

QDB uses an **append-only log** as its primary storage structure.

### Why append-only?

Append-only logs have well-understood failure modes.  A write that is interrupted mid-way cannot corrupt earlier data; the partial record is detected and discarded during recovery.  This is simpler and more reliable than in-place mutation of records.

### Planned on-disk layout

```
myqueue.qdb
├── Header (fixed size, page 0)
│   ├── Magic number
│   ├── Format version
│   ├── Page size
│   └── Root pointers (queue index, WAL state)
│
├── Queue index (B-tree or hash table of name → segment list)
│
└── Segment files (append-only message log)
    ├── segment-0000000001.qseg
    ├── segment-0000000002.qseg
    └── ...
```

Each **segment** is an append-only file.  Records are written sequentially.  A record contains:

- A fixed-size header: message ID, queue name hash, payload length, CRC32 checksum
- The raw payload bytes
- A commit marker (a trailing byte that is written last, making partial writes detectable)

**Compaction**: when all messages in a segment have been acknowledged, the segment file is deleted.  This keeps disk usage proportional to the number of unacknowledged messages rather than total messages ever written.

---

## 4. Write-Ahead Log and Crash Recovery **[Planned]**

To make multi-step operations (e.g. updating the queue index after writing a message) atomic and crash-safe, QDB uses a **write-ahead log (WAL)**.

### WAL protocol

1. Before modifying any index structure, write the intended change to the WAL and call `fsync`.
2. Apply the change to the primary data structure.
3. Write a WAL commit record and call `fsync`.
4. The WAL entry can now be discarded (lazily, at the next checkpoint).

### Recovery on `qdb_open()`

When `qdb_open()` finds an existing database file:

1. Read the WAL.
2. For each WAL entry that has a commit record, re-apply the change (idempotent).
3. For each WAL entry without a commit record, discard it (the operation never completed).
4. Truncate the WAL.

This is the standard ARIES-style recovery approach: **redo committed operations, discard uncommitted ones**.

### Pending-ack records

Messages returned by `qdb_pop()` are placed into a **pending-ack table** persisted in the WAL.  On recovery, any message whose pending-ack record has no corresponding `qdb_ack()` commit is reinserted into its queue.  This is what provides at-least-once delivery after a crash.

---

## 5. File Format Stability Policy

The on-disk format of a QDB database is considered a **stable public interface** from v1.0 onwards.

- A database created by QDB 1.x will be readable and writable by any QDB 1.y where y >= x.
- QDB 2.x will be able to read QDB 1.x databases in read-only mode, and will offer a migration path to the new format.
- The format version field in the database header allows the library to detect and reject incompatible formats gracefully.

Format changes before v1.0 are not subject to this guarantee.  The current pre-1.0 format is explicitly unstable.

---

## 6. Platform Portability

QDB targets Linux, macOS, and Windows.

All platform-specific code lives in `src/platform/` behind a thin abstraction layer (`qdb__file_sync`, `qdb__file_lock`, etc.).  The rest of the implementation uses only standard C17 and the platform abstraction layer.

Key portability considerations:

- `fsync` on Linux, `F_FULLFSYNC` on macOS (which provides a stronger guarantee than `fsync` on HFS+/APFS), `FlushFileBuffers` on Windows.
- File locking: `flock` / `fcntl` on POSIX, `LockFileEx` on Windows.
- Atomic rename: `rename()` on POSIX, `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` on Windows.

---

## 7. API Design Principles

- **Simple ownership**: callers own what they allocate.  QDB owns the memory it returns (e.g. `msg.data`); callers must not free it.
- **No callbacks on the critical path**: the API is synchronous.  Asynchronous wrappers are the caller's responsibility.
- **Opaque handles**: `qdb_t` is forward-declared and never exposed structurally.  This preserves ABI stability across patch releases.
- **Negative error codes**: `QDB_OK` is 0; all errors are negative integers.  Callers can `if (rc < 0)` to catch any error.

---

## 8. Non-Goals (Architectural)

The following are explicitly out of scope for v1 and will not be designed for:

- Network I/O of any kind
- Multi-process shared access to a single database file
- Encryption at rest
- Compression
- Pub/sub fan-out semantics
- Transactions spanning multiple queues
