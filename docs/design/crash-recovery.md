# QDB Crash Recovery

This document defines exactly what QDB guarantees after a crash, the WAL protocol that provides those guarantees, and the step-by-step recovery procedure that `qdb_open()` executes.

---

## 1. Failure Model

QDB is designed to tolerate the following failure scenarios:

| Scenario | Expected outcome |
|---|---|
| Process killed (SIGKILL, OOM) at any point | Full recovery; no data loss for committed writes |
| System crash (kernel panic, power loss) after `fsync` returns | Full recovery |
| System crash before `fsync` returns | The in-flight write is lost; no previously committed data is lost |
| Storage media bit-flip on a written block | Detected via CRC32; `QDB_ERR_CORRUPT` returned |
| Partial `pwrite()` (OS writes fewer bytes than requested) | Detected via commit marker + CRC; record treated as not committed |
| `fsync` lies (battery-backed write cache without proper power-loss handling) | **Out of scope.** QDB trusts `fsync` semantics. |

**What "committed" means:**  A write is committed when the WAL record for that write has:
1. Its full payload written to the WAL file.
2. Its commit marker (`0xAB`) written to the WAL file.
3. `fsync()` returned successfully on the WAL file descriptor.

After those three conditions hold, the data is durable regardless of any subsequent crash.

---

## 2. WAL Protocol

The Write-Ahead Log (`<path>-wal`) decouples the durability of a write from the checkpointing of that write into the main file.

### 2.1 Normal write sequence

```
┌──────────────────────────────────────────────────────────────┐
│ Write a record (push, lease, ack, etc.)                      │
│                                                              │
│  1. Build record bytes in memory.                            │
│  2. pwrite(wal_fd, record_without_marker, wal_end_offset)    │
│  3. fsync(wal_fd)                          ← crash point A   │
│  4. pwrite(wal_fd, commit_marker=0xAB, wal_end_offset+N-1)  │
│  5. fsync(wal_fd)                          ← crash point B   │
│  6. Update in-memory state                                   │
│  7. Return QDB_OK to caller                                  │
└──────────────────────────────────────────────────────────────┘
```

**Crash at point A:**  The WAL record payload has been written but not the commit marker.  On recovery, the record scan stops at this record (commit marker is `0x00`), the record is discarded, and the WAL is truncated here.  The caller's `push()` / `pop()` / `ack()` will have returned an error (the `fsync` at step 3 would have to return before crash A could be a crash-after-return scenario — but if the process is killed mid-syscall, the kernel handles this by not writing anything beyond what the OS already committed).  Either way: no committed data is lost, no uncommitted data appears.

**Crash at point B:**  The commit marker is written and durable.  The WAL record is fully committed.  On recovery, the record is replayed into the main file.  The operation is logically complete.

### 2.2 WAL checkpoint sequence

When the WAL is checkpointed into the main file:

```
┌──────────────────────────────────────────────────────────────┐
│ Checkpoint                                                   │
│                                                              │
│  1. Lock the write mutex.                                    │
│  2. For each committed WAL record (in order):               │
│       a. Append record to main file (same write protocol).  │
│       b. fsync(main_fd).                                     │
│  3. Update log_end_offset in main file header.              │
│  4. Update next_msg_id and next_lease_id in header.         │
│  5. fsync(main_fd).               ← crash point C           │
│  6. Delete the WAL file.          ← crash point D           │
│  7. Update QDB_FLAG_WAL_PRESENT in header flags.            │
│  8. fsync(main_fd).                                         │
│  9. Unlock the write mutex.                                  │
└──────────────────────────────────────────────────────────────┘
```

**Crash at point C:**  Main file has the records but the WAL file still exists.  On recovery: the WAL is replayed again into the main file.  Since appending is idempotent (the log will have duplicate records for these operations), recovery must detect and skip duplicate records.  See section 4.4 for deduplication.

**Crash at point D:**  WAL file has been deleted but `QDB_FLAG_WAL_PRESENT` is still set in the header.  On recovery: the flag is set, but the WAL file does not exist.  This is a valid state — recovery reads the flag, looks for the WAL file, does not find it, and proceeds with a log scan from the main file only.

**Rationale for writing `log_end_offset` before deleting the WAL:**  If the WAL is deleted before `log_end_offset` is updated, a subsequent crash leaves the main file with a valid end offset that may not include the checkpointed records (if they were written after the last `log_end_offset` update).  Writing `log_end_offset` in step 3 before deleting the WAL in step 6 ensures the main file is self-consistent.

---

## 3. Durability Guarantees

| Operation | Durable after... |
|---|---|
| `qdb_push()` returns `QDB_OK` | WAL `fsync` (step 5 of write sequence) |
| `qdb_pop()` (lease) returns `QDB_OK` | WAL `fsync` |
| `qdb_ack()` returns `QDB_OK` | WAL `fsync` |
| Checkpoint completes | Main file `fsync` (step 5 of checkpoint) |

A push that returned `QDB_OK` will be present in the queue after any crash.  An ack that returned `QDB_OK` will be permanent after any crash (the message will not be redelivered after recovery).

---

## 4. Recovery Procedure (`qdb_open()`)

The following is the exact sequence executed on every `qdb_open()` call.

### 4.1 Acquire the lock file

`flock(lock_fd, LOCK_EX | LOCK_NB)`.  If this fails with `EWOULDBLOCK`, return `QDB_ERR_LOCKED`.

**Rationale:**  No recovery operation is safe if another process is concurrently writing to the same file.  The lock must be acquired before any file is read.

### 4.2 Read and validate the file header

1. Open the main file.
2. Read the first 4096 bytes.
3. Validate `magic` bytes.  Mismatch → `QDB_ERR_CORRUPT`.
4. Validate `format_version`.  Too new → `QDB_ERR_CORRUPT`.
5. Validate `page_size == 4096`.  Mismatch → `QDB_ERR_CORRUPT`.
6. Validate `header_crc32`.  Mismatch → `QDB_ERR_CORRUPT`.
7. Record `log_start_offset`, `log_end_offset`, `next_msg_id`, `next_lease_id`, `flags`.

### 4.3 Check for WAL and replay if present

If `QDB_FLAG_WAL_PRESENT` is set in `flags` OR the WAL file (`<path>-wal`) exists:

1. Open the WAL file.  If it does not exist but the flag was set, the WAL was deleted after the flag write but before the flag was cleared — proceed without the WAL (no records to replay).
2. Read and validate the WAL header.  If the WAL `magic` is wrong or `format_version` mismatches, return `QDB_ERR_CORRUPT`.
3. Read `main_file_log_end_offset` from the WAL header.  This is the offset where WAL records will be appended into the main file.
4. Scan WAL records from the WAL's log start.  For each record with `commit_marker == 0xAB`:
    - Validate `payload_crc32`.  Mismatch → stop scan, truncate WAL here, proceed.
    - Append the record to the main file at the appropriate offset (see 4.4).
5. After all WAL records are replayed, execute the checkpoint cleanup (steps 3–8 from section 2.2).

### 4.4 Deduplication during WAL replay

It is possible that some WAL records were already checkpointed into the main file before the crash.  Replaying them again would produce duplicate records.

**Detection:** Each `RT_MSG_PUSH` record contains a `msg_id`.  Each `RT_MSG_LEASE` / `RT_MSG_ACK` / `RT_MSG_NACK` / `RT_MSG_EXPIRE` record contains a `msg_id` and a `lease_id`.  During log scan, the implementation builds a seen-set of `(record_type, msg_id, lease_id)` tuples from the main file.  When replaying WAL records, any tuple already in the seen-set is skipped.

**Simpler alternative:** Only append WAL records at offsets strictly greater than `main_file_log_end_offset` as recorded in the WAL header.  If `main_file_log_end_offset` matches the current main file size, the checkpoint was complete and no records need replay.  If the main file is shorter, append the missing records.  This avoids building a seen-set.

The simpler alternative is preferred for v1.  The seen-set approach handles edge cases where `log_end_offset` in the header was updated but the records weren't fully fsynced, but those cases are already covered by the commit marker check on the main file records.

### 4.5 Truncate partial tail records in the main file

After WAL replay (or if no WAL was present):

1. Seek to `log_end_offset`.
2. If the file is longer than `log_end_offset`, truncate the file to `log_end_offset`.  (This handles partial writes that occurred after the last committed `log_end_offset` update.)
3. Scan the log from `log_start_offset` to `log_end_offset`.  For each record:
    - Validate `commit_marker == 0xAB`.
    - Validate `payload_crc32`.
    - If either check fails, truncate the file at this record's start offset and stop.

**Note:** In a healthy database, step 3 should never find a corrupt record — the `log_end_offset` field should already exclude any partial records.  The scan is a belt-and-suspenders check that adds robustness against bugs in the writer.

### 4.6 Rebuild the in-memory index

Scan the log from `log_start_offset` to the (possibly truncated) end, processing each record:

| Record type | Action |
|---|---|
| `RT_MSG_PUSH` | Add `MsgRef` to back of `ready_msgs[queue_name]` |
| `RT_MSG_LEASE` | Move `MsgRef` from `ready_msgs` to `pending_acks`; push onto `lease_heap` |
| `RT_MSG_ACK` | Remove from `pending_acks` |
| `RT_MSG_NACK` | Move from `pending_acks` back to front of `ready_msgs` |
| `RT_MSG_EXPIRE` | Move from `pending_acks` back to front of `ready_msgs` |
| `RT_CHECKPOINT` | Update `next_msg_id` and `next_lease_id` lower bounds |
| `RT_PADDING` | Skip |

After the scan:
- `global_state.next_msg_id` = max(`header.next_msg_id`, highest seen `msg_id` + 1).
- `global_state.next_lease_id` = max(`header.next_lease_id`, highest seen `lease_id` + 1).

### 4.7 Evaluate expired leases

After rebuilding the index, scan `pending_acks` for messages whose `lease_expiry` has already passed (expired while the database was closed).

For each expired message:
1. Write `RT_MSG_EXPIRE` to the WAL.
2. Move the message to front of `ready_msgs`.

This ensures that messages that were popped but never acked in a previous session are immediately available for redelivery.

### 4.8 Set `QDB_FLAG_DIRTY`

Write `QDB_FLAG_DIRTY = 1` to the file header and `fsync`.  This flag is cleared in `qdb_close()`.  If the process is killed and the flag is set on the next open, recovery knows to perform a full scan regardless of whether the WAL is present.

### 4.9 Database is now open

Return the `qdb_t *` handle.  The database is in a consistent, recoverable state.

---

## 5. Clean Close

`qdb_close()` performs:

1. Checkpoint the WAL into the main file (if the WAL is non-empty).
2. Write an `RT_CHECKPOINT` record to the main log.
3. `fsync` the main file.
4. Clear `QDB_FLAG_DIRTY` and `QDB_FLAG_WAL_PRESENT` in the header.
5. `fsync` the main file header.
6. Close the main file descriptor.
7. Delete the WAL file (if it exists).
8. Release and close the lock file.

After a clean close, the next `qdb_open()` skips WAL recovery (no WAL present, dirty flag not set) and proceeds directly to the index rebuild.

---

## 6. Handling Corrupt Databases

If recovery detects corruption that cannot be automatically resolved (bad magic, bad header CRC, unknown format version), `qdb_open()` returns `QDB_ERR_CORRUPT` and leaves the file untouched.

QDB does **not** attempt to auto-repair corrupt databases.  The reasoning:

- Auto-repair risks making a bad situation worse (e.g. truncating valid data because it looks corrupt due to a bug in the repair logic).
- Corruption is a symptom of a deeper problem (hardware failure, filesystem bug, incorrect usage).  The operator needs to know about it.
- The `qdb inspect` tool (future) will provide offline repair and diagnostic utilities.

**The one exception:**  A partial tail record (detected by a missing commit marker) is silently truncated.  This is not corruption in the meaningful sense — it is a predictable consequence of process termination during a write, and the correct response is well-defined (discard the partial record).
