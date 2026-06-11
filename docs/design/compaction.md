# QDB Compaction Strategy

This document describes how QDB reclaims disk space occupied by dead records — records for messages that have been permanently acknowledged and will never be read again.

---

## 1. The Problem: Log Growth

The append-only log never shrinks on its own.  Every push, lease, ack, and expiry appends a new record.  For a long-running process that continuously pushes and acks messages, the log grows without bound even if the number of *live* messages (those in READY or LEASED state) is small or zero.

Specifically, a message that is pushed and immediately acked leaves two permanent records in the log:

```
RT_MSG_PUSH  (msg_id=1, queue="jobs", data=...)   ← 9 + 22 + data_len bytes
RT_MSG_ACK   (msg_id=1, lease_id=1)               ← 9 + 16 bytes
```

Without compaction, a process that pushes and acks one message per second would accumulate gigabytes of dead records over days.

---

## 2. The Dead Record Set

A record is **dead** if the message it refers to is in ACKED state — i.e. all of the following records exist in the log:
- An `RT_MSG_PUSH` for `msg_id = X`
- An `RT_MSG_LEASE` for `msg_id = X`
- An `RT_MSG_ACK` for `msg_id = X`

Additionally, `RT_MSG_LEASE`, `RT_MSG_EXPIRE`, and `RT_MSG_NACK` records for messages that have been acked are also dead.

A record is **live** if it belongs to a message currently in READY or LEASED state.

**Observation:** The in-memory index already tracks exactly which messages are live.  The set of dead records is everything in the log that is not referenced by the in-memory index.

---

## 3. Compaction Approach: Shadow-File Rewrite

QDB uses a **full-file rewrite** (shadow file) strategy:

1. Write a new file (`<path>-compact`) containing only live records.
2. Atomically rename `<path>-compact` over `<path>`.
3. The old file (now unlinked) is reclaimed by the OS when its file descriptor reference count reaches zero.

This is the simplest possible compaction strategy.  It has known, bounded behaviour: after compaction, the file contains exactly the live records and nothing else.

### 3.1 Compaction write protocol

The shadow file is written using the same record format as the main file:

1. Write a fresh file header with updated `next_msg_id`, `next_lease_id`, `log_start_offset=4096`, `log_end_offset` (to be filled in at the end), `create_time_us` = original file's `create_time_us` (preserved), `flags = 0`.
2. For each queue in alphabetical order, for each message in FIFO order:
    - If the message is READY or LEASED, write its `RT_MSG_PUSH` record.
    - If the message is LEASED, write its `RT_MSG_LEASE` record (with the current lease).
3. Write an `RT_CHECKPOINT` record at the end of the shadow file's log.
4. Update `log_end_offset` in the shadow file header.
5. Compute and write `header_crc32`.
6. `fsync` the shadow file.
7. `rename(<path>-compact, <path>)` (atomic on POSIX; see platform notes).
8. `fsync` the directory containing `<path>` (to make the rename durable).

**Why sort queues and messages?**  Alphabetical queue order and FIFO message order are deterministic.  Deterministic output makes the compacted file reproducible and easier to reason about in tests and diagnostics.

### 3.2 Handling the WAL during compaction

Compaction must be preceded by a WAL checkpoint.  The sequence is:

1. Lock the write mutex (pause all writes).
2. Checkpoint the WAL into the main file.
3. Run compaction (steps 1–8 above).
4. Unlock the write mutex.

Holding the write mutex for the duration of compaction means the database is unavailable for writes while the shadow file is being produced.  For most deployments (tens of thousands of live messages, a few hundred megabytes), this pause is measured in milliseconds.  For very large databases, future versions may implement an online compaction strategy.

**Alternative considered — online compaction (copy-on-write):**  Allow writes to continue while compaction runs.  New records go to a side buffer or WAL; the shadow file is written from the pre-compaction snapshot; new records are applied on top when the rename happens.  This is significantly more complex and is deferred to a future version.

### 3.3 After compaction: updating the in-memory index

After the rename, all `MsgRef.log_offset` values in the in-memory index are stale — they pointed to offsets in the old file.  The offsets in the compacted file are different.

The simplest correct approach: **rebuild the index from the compacted file**, exactly as `qdb_open()` does.  Since the write mutex is held and no writes have happened since the WAL checkpoint, the rebuild is guaranteed to produce exactly the same in-memory state, with updated log offsets.

The rebuild is O(live messages), which is cheap by definition immediately after compaction.

---

## 4. Compaction Trigger Policy

Compaction is triggered when either of the following thresholds is crossed:

| Trigger | Default threshold | Rationale |
|---|---|---|
| File size exceeds N × live data size | N = 4 | If 75% of the file is dead records, compact |
| File size exceeds absolute limit | 512 MiB | Cap worst-case disk usage |

The "N × live data size" trigger requires estimating live data size.  This is done cheaply from the in-memory index: sum `data_len` over all live messages.  The ratio `file_size / estimated_live_size` is checked after each checkpoint.

Neither trigger compacts automatically.  Instead, they set a `compaction_recommended` flag.  Compaction is then run:
- In `qdb_close()` if the flag is set.
- When the caller invokes `qdb_compact()` (future explicit API).

**Rationale for not compacting automatically during writes:**  Compaction holds the write mutex for a potentially noticeable pause.  Triggering this pause silently in the middle of a `qdb_push()` call would violate the principle of least surprise.  The caller should have control over when the pause happens.  A future version may offer a background compaction option.

**Alternative considered — always compact on close:**  Simple and predictable.  The downside is that a database with millions of live messages takes a long time to close.  The threshold approach compacts only when there is meaningful space to reclaim.

---

## 5. Crash Safety of Compaction

### 5.1 Crash before `rename`

The shadow file (`<path>-compact`) exists but the main file is unchanged.  On the next `qdb_open()`, the library sees the leftover shadow file and deletes it.  (The shadow file is never the source of truth until the rename succeeds.)

### 5.2 Crash after `rename` but before directory `fsync`

On most filesystems (ext4, APFS, NTFS), `rename` is atomic at the filesystem level.  If the system crashes after the `rename` call returns, the rename is considered durable on ext4 (with `data=ordered` mode) and APFS.  On older filesystems or with unusual mount options, the rename may not be durable.

The directory `fsync` at step 8 addresses this: it forces the directory entry update to disk before returning.  If the process is killed between the `rename` and the directory `fsync`, the OS may or may not have persisted the rename.  On the next open:
- If the rename was persisted: the main file is the compacted version.  Normal recovery proceeds.
- If the rename was not persisted: the main file is the pre-compaction version.  The shadow file (`<path>-compact`) exists.  The library deletes the shadow file and proceeds with the pre-compaction main file.

In both cases, no data is lost.

### 5.3 Crash after directory `fsync`

The rename is durable.  The compacted file is the main file.  Normal recovery proceeds.

---

## 6. Space Reclamation Estimate

For a workload that continuously pushes and immediately acks messages:

```
Message data:       1 KiB average
RT_MSG_PUSH record: 9 (frame) + 22 (fixed fields) + 255 (max queue name) + 1024 (data) ≈ 1.3 KiB
RT_MSG_LEASE:       9 + 24 = 33 bytes
RT_MSG_ACK:         9 + 16 = 25 bytes

Dead record set per message: ≈ 1.36 KiB
```

After 100,000 push-and-ack cycles, the log accumulates ~136 MiB of dead records.  Compaction reduces this to near zero (only live messages remain).  With zero live messages, the compacted file is 4096 bytes (header only).

---

## 7. What Compaction Does NOT Do

- **Compaction does not defragment the heap.**  QDB has no heap; there is nothing to defragment.
- **Compaction does not change message IDs.**  The `next_msg_id` counter is preserved in the compacted file header.  IDs are never reused.
- **Compaction does not modify live messages.**  A message that is in READY or LEASED state at the time of compaction is written verbatim into the compacted file.
- **Compaction does not run automatically in the background (v1).**  It is a synchronous, caller-controlled operation.

---

## 8. Future: Incremental (Segmented) Compaction

For deployments with very large numbers of live messages, a full-file rewrite may be too slow.  A future design could partition the log into fixed-size segments (e.g. 64 MiB each) and compact individual segments independently:

- Dead segments (all messages acked) are deleted outright.
- Partially-dead segments are rewritten into a single compacted segment.
- Live segments are untouched.

This reduces the compaction pause from O(total file size) to O(dead segment size).  The added complexity of segment management is deferred to a future version.
