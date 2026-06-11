# QDB Storage Engine — Design Documents

This directory contains the detailed design specifications for the QDB storage engine.  Each document covers one layer of the system.  Read them in order for a complete picture; each document builds on the vocabulary established by the previous one.

---

## Documents

| Document | What it covers |
|---|---|
| [file-format.md](file-format.md) | On-disk byte layout: file header, record wire format, sidecar files |
| [storage-model.md](storage-model.md) | Append-only log mechanics, in-memory index, write path |
| [message-lifecycle.md](message-lifecycle.md) | State machine from push to acknowledgement |
| [crash-recovery.md](crash-recovery.md) | WAL protocol, recovery procedure, durability guarantees |
| [compaction.md](compaction.md) | Log growth, dead-record reclamation, safe file rewriting |
| [queue-semantics.md](queue-semantics.md) | FIFO ordering, leases, at-least-once delivery, multi-consumer rules |

---

## Design Principles

These principles govern every decision in the documents below.  When a trade-off arises, apply them in priority order.

1. **A completed write is never lost.**  If `qdb_push()` returns `QDB_OK`, the message survives any subsequent crash.

2. **Corruption is detected, not silently propagated.**  Every record carries a checksum.  A partial write leaves a detectable sentinel; QDB will truncate it rather than expose garbage data.

3. **Recovery is automatic and requires no operator action.**  `qdb_open()` always leaves the database in a consistent state, regardless of when the previous run was interrupted.

4. **The file format is the contract.**  Internal data structures are implementation details.  The on-disk format is a stable public interface from v1.0 onwards.

5. **Simplicity over cleverness.**  A design that can be fully understood in one reading is preferred over a design that squeezes out extra performance.

---

## Glossary

| Term | Definition |
|---|---|
| **Record** | A single variable-length entry in the log: one push, one ack, one lease, etc. |
| **Log** | The append-only sequence of all records ever written, starting after the file header |
| **Log offset** | Byte position within the file, measured from the start of the file |
| **Message ID** | A monotonically increasing `uint64_t` assigned at push time; globally unique within a database |
| **Queue** | A named, ordered sequence of messages.  Names are arbitrary UTF-8 strings up to 255 bytes |
| **Lease** | A time-bounded exclusive claim on a message granted by `qdb_pop()` |
| **ACK** | An explicit acknowledgement that a leased message was processed; makes deletion permanent |
| **NACK** | An explicit rejection that returns a leased message to the front of its queue immediately |
| **WAL** | Write-ahead log: a sidecar file used to make multi-step updates atomic |
| **Checkpoint** | The act of flushing the WAL into the main file and resetting the WAL |
| **Compaction** | Rewriting the main file to discard records for messages that have been permanently deleted |
| **Dead record** | A record whose message has been acknowledged; it occupies space but carries no live data |
| **Shadow file** | The temporary file written during compaction, atomically renamed over the main file when complete |
