# QDB Storage Model

This document describes the append-only log mechanics, the in-memory index that QDB maintains for fast queue access, and the write path from API call to durable storage.

---

## 1. The Append-Only Log

The QDB log is a sequential, append-only file.  Records are always written at the end.  No existing record is ever modified or deleted in place.

This is the central design choice from which everything else follows.

### Why append-only?

**Failure simplicity.**  In an append-only log, the only way a write can be partial is at the tail.  The library always knows exactly where the tail is (from `log_end_offset` in the header) and can detect and truncate a partial tail record on every open.  In an in-place update design, a partial write can corrupt any arbitrary record in the middle of the file — detection requires checksums on every block, and recovery may require complex rollback logic.

**Predictable I/O pattern.**  Sequential writes are the fastest pattern for every storage medium (spinning disk, SSD, NVMe, network-attached storage).  The OS can predict and prefetch ahead.  There is no write amplification from updating B-tree pages scattered across the file.

**Audit trail.**  Every state transition (push, lease, ack, expire) is a permanent record in the log.  The full history of every message can be reconstructed by scanning the log.  This is useful for debugging and for future tooling (e.g. a `qdb inspect` utility).

**Simplicity of the write path.**  A push is a single sequential write.  There is no need to locate a free page, update a page directory, handle overflow pages, or maintain a free list.

### The cost of append-only

The log grows without bound until compaction runs.  A message that was pushed and acknowledged immediately still leaves two records (one `RT_MSG_PUSH`, one `RT_MSG_ACK`) permanently in the log until the next compaction.  The compaction strategy (see [compaction.md](compaction.md)) addresses this.

---

## 2. In-Memory Index

The on-disk log is the source of truth.  But scanning the full log on every `qdb_pop()` would be O(log size), not O(1).  QDB builds and maintains an in-memory index for fast access.

### 2.1 Index structures

```
per_queue_index:
    map<queue_name: string → queue_state: QueueState>

QueueState:
    ready_msgs:   deque<MsgRef>      // FIFO order; front = oldest
    pending_acks: map<msg_id: u64 → PendingMsg>

MsgRef:
    msg_id:        u64
    log_offset:    u64               // byte offset of RT_MSG_PUSH record in main file
    data_len:      u32

PendingMsg:
    msg_ref:       MsgRef
    lease_id:      u64
    lease_expiry:  u64               // unix microseconds

global_state:
    next_msg_id:   u64
    next_lease_id: u64
    lease_heap:    min-heap<(expiry_us: u64, msg_id: u64, queue_name: string)>
```

The index is never persisted to disk.  It is rebuilt entirely from the log on every `qdb_open()`.  The correctness of the index is a consequence of the correctness of the log.

**Rationale for not persisting the index:**  Persisting a separate index file introduces a second file that must be kept in sync with the log.  Any inconsistency between the log and the index (e.g. if the index write succeeds but the log write does not, or vice versa) creates a corrupt state.  Rebuilding from the log at startup eliminates this class of bug entirely.  The startup cost is O(live messages) — acceptable for the target use-cases.

**Alternative considered — embedded B-tree index (SQLite style):**  A persistent B-tree would allow sub-linear startup even with millions of historical records.  Rejected for v1 because: (a) most deployments have hundreds to thousands of live messages, not millions; (b) a B-tree implementation is a significant increase in code complexity; (c) the append-only log already provides O(1) amortised access via the in-memory index once startup is complete.

### 2.2 Memory cost

Each `MsgRef` is approximately 20 bytes.  Each `PendingMsg` is approximately 40 bytes.  For 100,000 live messages, the index consumes roughly 2 MB of resident memory — negligible for any environment where QDB is applicable.

### 2.3 Index rebuild at startup

See [crash-recovery.md](crash-recovery.md) for the full startup sequence.  In summary:

1. Read and validate the file header.
2. Apply any pending WAL records.
3. Scan the log from `log_start_offset` to `log_end_offset`, building the index.
4. The index after the scan reflects the exact state of the queue as of the last committed record.

---

## 3. Write Path

### 3.1 `qdb_push()` — full write path

```
Application calls qdb_push(db, "jobs", data, len)
│
├─ 1. Validate arguments (queue name, data pointer, length)
│
├─ 2. Acquire the next msg_id
│      new_id = db->global_state.next_msg_id
│      db->global_state.next_msg_id++          ← in memory only at this point
│
├─ 3. Build the RT_MSG_PUSH record in a stack/heap buffer
│      record = { type=RT_MSG_PUSH, payload={ msg_id=new_id, queue=..., data=... } }
│      Compute payload_crc32
│      Set commit_marker = 0x00  (not yet committed)
│
├─ 4. Stage the record in the WAL
│      a. pwrite(wal_fd, record, record_size, wal_end_offset)
│      b. fsync(wal_fd)
│         ← CRASH HERE: WAL has record but no commit marker.
│            Recovery discards this record. Safe.
│      c. pwrite(wal_fd, commit_marker=0xAB, 1, wal_end_offset + record_size - 1)
│      d. fsync(wal_fd)
│         ← CRASH HERE: WAL record is committed.
│            Recovery replays it into main file. Safe.
│
├─ 5. Update the in-memory index
│      db->per_queue["jobs"].ready_msgs.push_back(MsgRef{new_id, ...})
│
├─ 6. Return QDB_OK to the caller
│
└─ (Background / lazy) Checkpoint: flush WAL records into main file
```

### 3.2 The WAL as the commit boundary

The push is considered durable — and `QDB_OK` is returned to the caller — as soon as step 4d completes (the WAL record has its commit marker, and `fsync` has returned).

The caller never needs to wait for the WAL to be checkpointed into the main file.  Checkpointing is a background operation that happens lazily (or when the WAL reaches a size threshold).

**Rationale:**  This is the same model used by PostgreSQL (`fsync` on WAL commit, async checkpoint to main heap file) and SQLite WAL mode.  The WAL `fsync` is the only mandatory durability barrier on the hot path.

### 3.3 WAL checkpoint trigger

The WAL is flushed into the main file (checkpointed) when:

1. The WAL exceeds a size threshold (default: 1 MiB).
2. `qdb_close()` is called.
3. The application calls `qdb_checkpoint()` explicitly (future API).

During a checkpoint:
1. Lock the checkpoint mutex (preventing concurrent writes during the process).
2. For each WAL record (in order), append it to the main file using the normal record write protocol.
3. Update `log_end_offset` in the file header.
4. Update `next_msg_id` in the file header.
5. `fsync` the main file.
6. Delete the WAL file.
7. Clear `QDB_FLAG_WAL_PRESENT` in the header.
8. `fsync` the main file header.

If the process crashes during step 2–6, the WAL file still exists and will be replayed on the next open.  The replay is idempotent.

### 3.4 `qdb_pop()` — full write path

```
Application calls qdb_pop(db, "jobs", &msg)
│
├─ 1. Check db->per_queue["jobs"].ready_msgs — if empty, return QDB_ERR_EMPTY
│
├─ 2. Peek at the front of ready_msgs:  MsgRef ref = ready_msgs.front()
│
├─ 3. Assign a lease
│      new_lease_id = db->global_state.next_lease_id++
│      expiry       = now_us() + db->config.lease_duration_us
│
├─ 4. Write RT_MSG_LEASE to WAL (same 4-step protocol as push)
│
├─ 5. Update the in-memory index
│      ready_msgs.pop_front()
│      pending_acks[ref.msg_id] = PendingMsg{ref, new_lease_id, expiry}
│      lease_heap.push({expiry, ref.msg_id, "jobs"})
│
├─ 6. Read the message payload from the main file
│      pread(main_fd, buf, ref.data_len, ref.log_offset + payload_data_offset)
│
├─ 7. Populate *msg and return QDB_OK
│      msg->id   = ref.msg_id
│      msg->data = buf               ← buffer owned by qdb_t, valid until next call
│      msg->len  = ref.data_len
│
└─ Return QDB_OK
```

Note that `qdb_pop()` reads the payload from the main file at step 6, not from the WAL.  For newly pushed messages that haven't been checkpointed yet, the payload is in the WAL, so the read must check the WAL first (if the message log offset falls within the WAL's address range, read from the WAL; otherwise read from the main file).

### 3.5 `qdb_ack()` — full write path

```
Application calls qdb_ack(db, msg_id)
│
├─ 1. Look up msg_id in pending_acks — if not found, return QDB_ERR_NOENT
│
├─ 2. Write RT_MSG_ACK to WAL (with msg_id and lease_id)
│
├─ 3. Remove from pending_acks
│
└─ Return QDB_OK
```

### 3.6 Lease expiry — background path

A background timer (or a check at the start of each `qdb_pop()`) evaluates the `lease_heap`:

```
while lease_heap.top().expiry_us <= now_us():
    entry = lease_heap.pop()
    msg   = pending_acks[entry.msg_id]

    if msg.lease_id != entry.lease_id:
        continue  // stale heap entry; message was already acked or re-leased

    Write RT_MSG_EXPIRE to WAL

    pending_acks.remove(entry.msg_id)
    ready_msgs.push_front(MsgRef{...})  // returned to front of queue
```

Expired messages go to the **front** of the ready queue, not the back.  Rationale: a message that was popped but not processed is more urgent than a message that has not yet been attempted.  Returning it to the front preserves approximate ordering.

---

## 4. Read Path

### 4.1 `qdb_pop()` — reading message payload

The message payload is stored in the `data` field of an `RT_MSG_PUSH` record at a known log offset (`m->data_file_offset`).  The read is a single `pread()` from that offset.

`qdb_pop()` allocates a fresh heap buffer for `out_msg->data` and copies the bytes into it.  It similarly heap-allocates `out_msg->queue`.  Both are owned by the caller and must be released with `qdb_msg_free()`.

**Ownership:** `out_msg->data` and `out_msg->queue` survive any subsequent call on the same handle.  The caller decides when to free them.  A zero-initialised `qdb_msg_t` is always safe to pass to `qdb_msg_free()`.

---

## 5. Concurrency Model

QDB v1 is **single-threaded per handle**.  A `qdb_t` must not be used concurrently from multiple threads without external locking.

This is not a limitation of the design but a deliberate scope restriction for v1.  The log-append model is compatible with a future multi-threaded design (a write lock on the tail, read locks on individual queue states), but that complexity is deferred.

The file-level lock (via the lock file) prevents two processes from opening the same database simultaneously.

---

## 6. fsync Strategy

Every durable write follows this sequence:

1. `pwrite()` the data bytes (not including commit marker).
2. `fsync()` (or `fdatasync()` where sufficient).
3. `pwrite()` the commit marker byte (`0xAB`).
4. `fsync()`.

Step 2 ensures the payload bytes are durable before the commit marker is visible.  Without step 2, the OS could reorder the writes and make the commit marker visible while the payload is still in a write-back cache — leaving a record that looks committed but has garbage payload bytes.

**Platform notes:**
- **Linux:**  `fdatasync()` is used when only the data (not metadata) needs to be flushed, saving the overhead of updating inode modification time.  The file header writes use `fsync()` because they change file size (metadata).
- **macOS:**  `fcntl(fd, F_FULLFSYNC)` is used instead of `fsync()`.  On macOS, `fsync()` only flushes to the OS buffer cache, not to the disk write cache.  `F_FULLFSYNC` issues a hardware flush command to the drive, providing true durability.  This is the documented approach in Apple's Technical Q&A QA1811.
- **Windows:**  `FlushFileBuffers()` is the equivalent of `fsync()`.

The platform abstraction (`src/platform/qdb_sync.c`) hides these differences behind a single `qdb__file_sync(fd)` function.
