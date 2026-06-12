# QDB Reliability Model

This document explains how QDB achieves its durability and delivery guarantees:
what "crash-safe" means in practice, how leases work, and what the ACK / NACK /
expiry operations do to message state.

---

## The append-only log

Every state change is recorded as a new record appended to the end of the log
file.  Records are never modified or deleted in place.  This is the foundation
from which all other guarantees follow.

The five record types and the transitions they record:

| Record | Transition |
|---|---|
| `RT_MSG_PUSH` | (none) → PENDING |
| `RT_MSG_LEASE` | PENDING → LEASED |
| `RT_MSG_ACK` | LEASED → ACKED |
| `RT_MSG_NACK` | LEASED → PENDING |
| `RT_MSG_EXPIRE` | LEASED → PENDING |

The log is the single source of truth.  The in-memory hash tables and FIFO
lists are a cache rebuilt entirely from the log on every `qdb_open()`.

---

## Durable-first mutation ordering

Every operation follows a strict ordering to ensure the in-memory state is
never ahead of the durable state:

```
1. Validate inputs entirely in memory.
   (Return error without touching disk if anything is wrong.)

2. Write the record to the log file (payload bytes, then fsync).

3. Write the commit marker byte (0xAB) to the log, then fsync again.
   (The two-fsync protocol ensures the payload is durable before the
   commit marker is visible; see "Two-phase durable write" below.)

4. Update the file header (log_end_offset, etc.), then fsync.

5. Only now: mutate the in-memory hash tables and FIFO lists.
```

If the process is killed at any point before step 5, the in-memory state has
not changed.  If the process is killed between steps 3 and 5, the record is
on disk; the next `qdb_open()` replays it and reconstructs the correct state.

**Rule:** if a disk write fails, the function returns an error and _nothing_ in
memory has changed.  The caller may close and reopen the database to get a
consistent in-memory state.

---

## Two-phase durable write

Each record is written in two phases to guarantee the payload is visible if and
only if the commit marker is visible:

```
Phase 1:  pwrite(record_header + payload)
          fsync / fdatasync / F_FULLFSYNC
          ← crash here: record has no commit marker; truncated on next open

Phase 2:  pwrite(commit_marker = 0xAB at record_end)
          fsync
          ← crash here: record is fully committed; replayed on next open
```

Without the phase-1 fsync, the OS could reorder the writes and make the commit
marker durable before the payload, leaving a "committed" record with garbled
bytes.

**Platform notes:**
- **Linux:** `fdatasync()` is used for payload writes (data only); `fsync()`
  is used for header writes (metadata change).
- **macOS:** `fcntl(fd, F_FULLFSYNC)` replaces `fsync()`.  Apple's `fsync()`
  only flushes to the kernel buffer cache, not to the drive.  `F_FULLFSYNC`
  issues a hardware flush and provides true durability.
- **Windows:** `FlushFileBuffers()` is the equivalent.

---

## CRC-32 corruption detection

Every record payload is protected by a CRC-32/ISO-HDLC checksum stored in the
record header.  On replay, the checksum is recomputed and compared.  A mismatch
causes `qdb_open()` to return `NULL` with `QDB_ERR_CORRUPT`.

The file header (4 096 bytes) has its own CRC-32 covering the first 52 bytes.
An invalid header checksum also causes `qdb_open()` to refuse to open the file.

QDB does not silently discard corrupt records.  When corruption is detected,
the database is considered unreadable and the application must handle it
explicitly (restore from backup, alert an operator, etc.).

---

## At-least-once delivery

A message returned by `qdb_pop()` is guaranteed to be delivered **at least
once** even across process crashes.

The sequence:

1. `qdb_pop()` writes a durable `RT_MSG_LEASE` record and transitions the
   message to LEASED state.

2. The caller processes the message and calls `qdb_ack()` on success.

3. If the caller crashes between steps 1 and 2 (or calls neither `qdb_ack()`
   nor `qdb_nack()` before closing), the lease record is in the log.

4. On the next `qdb_open()` the log is replayed.  The `RT_MSG_LEASE` record
   is replayed and the message is left in LEASED state with its original
   lease deadline.

5. Once `qdb_process_expired_leases()` is called and the deadline has passed,
   the message is written as `RT_MSG_EXPIRE` and returned to PENDING.

6. The next `qdb_pop()` redelivers the message.

The `retry_count` field tracks how many times a message has been NACKed or
expired.  It is incremented by each `qdb_nack()` and `qdb_process_expired_leases()`
call and is replayed correctly on restart.

**Implication:** callers must be prepared to receive the same message more than
once.  Idempotent processing or an external deduplication key is the standard
solution.

---

## Leases

A **lease** is a time-bounded exclusive claim on a message.

When `qdb_pop()` is called:
- A new `lease_id` (monotonically increasing uint64) is assigned.
- A lease deadline is computed: `now_us + QDB_DEFAULT_LEASE_US` (30 seconds).
- An `RT_MSG_LEASE` record containing `msg_id`, `lease_id`, and `expiry_us`
  is written durably.
- The message transitions from PENDING to LEASED.
- The `qdb_msg_t` returned to the caller contains both `msg.id` and
  `msg.lease_id`.

The lease has three possible resolutions:

| Resolution | API call | Effect |
|---|---|---|
| Acknowledged | `qdb_ack(db, msg.id, msg.lease_id)` | Message → ACKED; never redelivered |
| Negatively acknowledged | `qdb_nack(db, msg.id, msg.lease_id)` | Message → PENDING at queue tail; lease removed |
| Expired | `qdb_process_expired_leases(db)` | Message → PENDING at queue tail; lease removed |

The `lease_id` parameter on `qdb_ack()` and `qdb_nack()` is a safety check.
If a lease expires, the message is re-leased to a new caller with a new
`lease_id`.  The original caller's stale `msg.lease_id` will then be rejected
by `qdb_ack()` / `qdb_nack()` with `QDB_ERR_INVAL`, preventing a slow consumer
from accidentally acknowledging a message that is already being processed by
another consumer.

---

## ACK

`qdb_ack(db, msg_id, lease_id)` permanently removes a message from the active
queue.

Steps:
1. Verify the message exists, is LEASED, and `m->lease_id == lease_id`.
2. Write `RT_MSG_ACK` durably.
3. Update `log_end_offset` in the file header.
4. Set `m->state = ACKED`, zero the lease fields, remove the lease table entry,
   and decrement `q->leased_count`.

After ACK, the message record remains in the log (for audit and replay
correctness) but is invisible to `qdb_pop()`.  Compaction will eventually
reclaim the space.

---

## NACK

`qdb_nack(db, msg_id, lease_id)` releases the lease and returns the message to
the tail of the queue.

Steps are identical to ACK up to step 4, where instead:
- `m->state = PENDING`.
- `m->lease_id = 0`, `m->lease_expiry_us = 0`.
- `m->retry_count++`.
- Message is appended to the tail of the queue's PENDING list.

Messages go to the **tail** on NACK, not the head.  Rationale: messages ahead
of the NACKed message in the queue have been waiting longer and should be
processed first.

---

## Expiry

`qdb_process_expired_leases(db)` is semantically equivalent to calling
`qdb_nack()` on every message whose `lease_expiry_us < now_us`.

It writes one `RT_MSG_EXPIRE` record per expired lease.  The `RT_MSG_EXPIRE`
payload format is identical to `RT_MSG_ACK` / `RT_MSG_NACK` (16 bytes:
`msg_id` + `lease_id`), and it is replayed identically to `RT_MSG_NACK`:
message returns to PENDING at the tail, `retry_count` incremented.

The distinction between NACK and EXPIRE records is intentional:
- NACK means the consumer explicitly returned the message.
- EXPIRE means the consumer did not respond in time (crashed, stalled, etc.).

Future observability tooling can use this distinction in audit logs.

---

## Replay consistency checks

On `qdb_open()`, the replay engine enforces these invariants.  Any violation
returns `QDB_ERR_CORRUPT`:

- A PUSH record with `msg_id == 0` is rejected (0 is the sentinel for "no
  neighbour" in the intrusive linked list).
- A PUSH record with a `msg_id` already in the message table is a duplicate
  and is rejected.
- A LEASE record for an unknown message ID is rejected.
- A LEASE record for a message that is not PENDING (already LEASED or ACKED)
  is rejected.
- An ACK / NACK / EXPIRE record for an unknown message ID is rejected.
- An ACK / NACK / EXPIRE record for a message that is not LEASED is rejected.
- An ACK / NACK / EXPIRE record whose `lease_id` does not match the
  message's active `lease_id` is rejected.
