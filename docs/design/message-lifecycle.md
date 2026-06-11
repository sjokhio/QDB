# QDB Message Lifecycle

This document defines the complete lifecycle of a message: every state it can be in, every transition that moves it between states, what log records are written at each transition, and what the rules are for each state.

---

## 1. State Machine

A message exists in exactly one of four states at any point in time:

```
                    qdb_push()
                        │
                        ▼
                   ┌─────────┐
                   │  READY  │◄──────────────────────────┐
                   └─────────┘                           │
                        │                                │
                   qdb_pop()                    lease expired
                        │                      OR qdb_nack()
                        ▼                                │
                   ┌─────────┐                           │
                   │ LEASED  │───────────────────────────┘
                   └─────────┘
                        │
                   qdb_ack()
                        │
                        ▼
                   ┌─────────┐
                   │  ACKED  │
                   └─────────┘
                        │
               (compaction removes
                the push record)
                        │
                        ▼
                   ┌─────────┐
                   │  GONE   │
                   └─────────┘
```

| State | Description | In-memory location | On-disk evidence |
|---|---|---|---|
| **READY** | Queued, waiting for a consumer | `ready_msgs` deque | `RT_MSG_PUSH` record, no matching `RT_MSG_LEASE` |
| **LEASED** | Claimed by a consumer; awaiting ACK or expiry | `pending_acks` map | `RT_MSG_PUSH` + `RT_MSG_LEASE`, no matching `RT_MSG_ACK` |
| **ACKED** | Processing confirmed; pending compaction | Neither (removed from index) | `RT_MSG_PUSH` + `RT_MSG_LEASE` + `RT_MSG_ACK` |
| **GONE** | Physically removed from the file by compaction | Neither | No records remain |

The `ACKED` state is a transitional state that exists only on disk, not in memory.  Once a message is acked, the in-memory index forgets it.  The on-disk records remain until compaction removes them.

---

## 2. Transition: PUSH (→ READY)

**Trigger:** `qdb_push(db, queue, data, len)`.

**Pre-conditions:** `db` is open; `queue` is a valid name; `data` is non-NULL; `len` is in [1, QDB_MSG_MAX_LEN].

**Actions:**
1. Assign `msg_id = next_msg_id; next_msg_id++`.
2. Write `RT_MSG_PUSH` record to WAL with `msg_id`, queue name, and payload.
3. Append `MsgRef{msg_id, log_offset, data_len}` to `ready_msgs[queue]`.

**Post-conditions:** The message is durable (WAL `fsync` completed).  It will appear at the back of its queue.

**Failure modes:**
- If the WAL write or `fsync` fails, the in-memory index is not updated and `QDB_ERR_IO` is returned.  `next_msg_id` is not incremented (the ID was never committed).  The partially written WAL record is discarded on the next open.
- If the process crashes after the WAL `fsync` but before the in-memory update, the index is rebuilt from the log on the next open — the message appears in READY state correctly.

---

## 3. Transition: POP (READY → LEASED)

**Trigger:** `qdb_pop(db, queue, &msg)`.

**Pre-conditions:** `db` is open; `queue` is a valid name; `ready_msgs[queue]` is non-empty.

**Actions:**
1. Peek `ref = ready_msgs[queue].front()`.
2. Assign `lease_id = next_lease_id; next_lease_id++`.
3. Compute `expiry = now_us() + lease_duration_us`.
4. Write `RT_MSG_LEASE` record to WAL with `msg_id`, `lease_id`, `expiry`.
5. Move `ref` from `ready_msgs` to `pending_acks[msg_id]`.
6. Push `(expiry, msg_id, queue)` onto the `lease_heap`.
7. Read payload from file; populate `*msg`.

**Post-conditions:** The consumer receives the message payload.  The message is in LEASED state.  The lease is durable — if the process crashes immediately, the message will be in LEASED state on recovery (and then transition back to READY when the lease expiry is evaluated).

**Failure mode:** If the WAL write fails, `QDB_ERR_IO` is returned and the message remains in READY state.

---

## 4. Transition: ACK (LEASED → ACKED)

**Trigger:** `qdb_ack(db, msg_id)`.

**Pre-conditions:** `msg_id` is in `pending_acks`.

**Actions:**
1. Look up `pm = pending_acks[msg_id]`.
2. Write `RT_MSG_ACK` record to WAL with `msg_id` and `pm.lease_id`.
3. Remove `msg_id` from `pending_acks`.

**Post-conditions:** The message is permanently consumed.  It will never be redelivered.  The log contains records for this message (push + lease + ack) until the next compaction.

**Why `lease_id` is included in the ACK:**  A consumer could hold a message for a long time, the lease could expire (causing the message to be re-leased to another consumer), and then the original consumer sends an ACK for its stale lease.  Including `lease_id` in the ACK allows the implementation to detect this scenario and return `QDB_ERR_NOENT` to the stale acker rather than incorrectly acking the message.

---

## 5. Transition: NACK (LEASED → READY)

**Trigger:** `qdb_nack(db, msg_id)`.  (Future API — not in the initial `qdb_pop` signature but designed for here.)

**Pre-conditions:** `msg_id` is in `pending_acks`.

**Actions:**
1. Write `RT_MSG_NACK` record to WAL.
2. Move `pm.msg_ref` from `pending_acks` back to the **front** of `ready_msgs[queue]`.
3. Remove the stale entry from `lease_heap` (lazily — the heap check filters it by `lease_id`).

**Rationale for returning to the front:**  The consumer explicitly rejected the message.  This typically means a transient processing failure (e.g. a downstream service is temporarily unavailable).  Returning the message to the front minimises the delay before a retry.  If multiple messages have been NACKed, they re-queue in the order they were originally popped (approximately).

**Alternative considered — return to back of queue:**  Avoids head-of-line blocking when one message is consistently failing.  This is a valid policy; it could be offered as a `qdb_nack_back()` variant in a future version.

---

## 6. Transition: EXPIRE (LEASED → READY)

**Trigger:** The current time exceeds `pm.lease_expiry` for a message in `pending_acks`.

**Who checks:** Lease expiry is evaluated:
- At the start of every `qdb_pop()` call (pop the front of the heap while the top is expired).
- In a future background thread / timer callback.

**Actions:**
1. Write `RT_MSG_EXPIRE` record to WAL.
2. Move `pm.msg_ref` back to the **front** of `ready_msgs[queue]`.
3. Remove from `pending_acks`.

**Post-conditions:**  The message is redeliverable.  The next `qdb_pop()` on this queue will return it.

**Default lease duration:**  30 seconds.  Configurable per `qdb_open()` via options (future API).  The lease duration is not stored in the database file — it is a runtime policy.

**Rationale for a lease duration rather than infinite retention:**  Without leases, a consumer that crashes after popping a message but before acking it holds the message permanently.  The lease ensures eventual redelivery even when consumers crash.  This is the mechanism that makes QDB provide at-least-once (rather than at-most-once) delivery semantics.

**Alternative considered — consumer heartbeat:**  The consumer sends periodic "I'm still working on this" signals, and the lease is considered live as long as heartbeats arrive.  More flexible (supports long-running jobs), but adds API complexity and requires a background I/O path.  The simple timeout lease is preferable for v1.

---

## 7. Transition: COMPACT (ACKED → GONE)

**Trigger:** Compaction runs (see [compaction.md](compaction.md)).

**Actions:**  The `RT_MSG_PUSH`, `RT_MSG_LEASE`, and `RT_MSG_ACK` records for this message are omitted from the rewritten log.

**Post-conditions:**  The message no longer exists anywhere.  Its `msg_id` will never be reused (the monotonic counter never decreases).

---

## 8. Message ID Properties

Message IDs are `uint64_t` values with the following properties:

- **Monotonically increasing within a database.**  If message B was pushed after message A, `B.msg_id > A.msg_id`.
- **Never reused.**  Even after a message is acked and compacted away, its ID is never assigned to a new message.
- **Unique per database.**  IDs are not globally unique across databases.
- **Not necessarily contiguous.**  IDs may have gaps if a push was started (ID allocated) but failed before the WAL commit.

### Why start at 1?

ID `0` is reserved as a sentinel / "no message" value.  Any code that zero-initialises a `qdb_msg_t` will have `id == 0`, which is guaranteed to be an invalid message ID.

### ID space exhaustion

At one million pushes per second (far beyond any expected throughput), a `uint64_t` counter wraps after approximately 584,000 years.  This is not a practical concern.

---

## 9. Delivery Guarantee: At-Least-Once

QDB provides **at-least-once delivery**: a message that has been pushed will be delivered to a consumer at least once before it is permanently deleted.

It does **not** provide exactly-once delivery.  A message may be delivered more than once if:
1. The consumer pops the message, processes it, but crashes before acking.
2. The lease expires, the message is redelivered.

Consumers must be prepared for duplicate delivery.  The `msg_id` field is a stable identifier that consumers can use to implement idempotent processing (deduplicate on their end).

**Why not exactly-once?**  True exactly-once delivery requires a distributed transaction between the queue and the consumer's processing logic — the consumer must atomically mark the message as processed and ack the queue.  QDB deliberately avoids this complexity.  At-least-once with idempotent consumers is the pragmatic and widely-used pattern.

---

## 10. Message Ordering Guarantee

Within a single queue, QDB provides **FIFO ordering**: messages are delivered in the order they were pushed.

Specifically: if `qdb_push(db, "q", A)` returns `QDB_OK` before `qdb_push(db, "q", B)` is called, then A will be returned by `qdb_pop()` before B, on any consumer that uses the same database handle.

This guarantee applies to the **first delivery** of each message.  After a redelivery (following a lease expiry or NACK), a message may be returned to the front of the queue, temporarily preceding messages that were pushed after it.  This is the expected behaviour — a redelivered message is more urgent than a fresh one.

**What ordering is NOT guaranteed:**
- Ordering across different queues.
- Total ordering across multiple processes (v1 is single-process only).
- Strict FIFO after redelivery (as described above).
