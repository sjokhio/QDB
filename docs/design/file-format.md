# QDB File Format

This document specifies the exact on-disk layout of a QDB database.  Every byte is accounted for.  Implementations must conform to this specification; the format is the contract.

---

## 1. Overview

A QDB database consists of at most three files that always share a common base path:

| File | Purpose |
|---|---|
| `<path>` | Main database file: file header + append-only log |
| `<path>-wal` | Write-ahead log: uncommitted changes, present only during a write or after a crash |
| `<path>-lock` | Advisory lock file: present whenever a process has the database open |

The main file and the WAL are described in full below.  The lock file has no defined contents; its existence on disk is its only meaning.

**Rationale for three files vs. one:**  A single-file design (where the WAL is embedded at a fixed offset) requires updating the file header on every checkpoint and complicates the invariant that the main file is always self-consistent.  Three files let each file have a single responsibility.  The tradeoff is that callers must treat all three as a unit when copying or moving a database — this is documented explicitly in the API.

**Alternative considered — segment directory:**  Kafka and similar systems store each log segment as a separate file inside a directory.  This simplifies compaction (delete a segment file) but requires directory-level atomicity guarantees that vary across filesystems and makes the database harder to copy as a unit.  A single main file is more consistent with SQLite's approach and the "drop one file into your project" goal.

---

## 2. Byte Order and Alignment

All multi-byte integers are stored in **little-endian** byte order.

**Rationale:**  x86, x86-64, and ARM (in its dominant configurations) are natively little-endian.  Using the native order avoids byte-swap overhead on the most common platforms.  SQLite uses big-endian for historical reasons (1990s SPARC workstations); QDB makes the modern choice.

**Alternative considered — big-endian:**  Big-endian ("network byte order") is traditional for binary file formats and improves human readability with a hex editor.  Rejected because the performance benefit of avoiding byte swaps on all modern hardware outweighs the minor readability advantage.

Records are **not** padded for alignment.  All fields are packed.  This simplifies the format at the cost of requiring `memcpy` on platforms that fault on unaligned reads.  All reference implementations must use `memcpy` to read multi-byte fields, never casting a pointer directly.

---

## 3. Main File Layout

```
Offset 0
┌──────────────────────────────────────┐
│  File Header          (4096 bytes)   │
├──────────────────────────────────────┤
│  Log Record 0                        │
├──────────────────────────────────────┤
│  Log Record 1                        │
├──────────────────────────────────────┤
│  ...                                 │
├──────────────────────────────────────┤
│  Log Record N                        │
└──────────────────────────────────────┘
EOF
```

The file header is always exactly 4096 bytes.  Log records begin at offset 4096 and extend to the end of the file.  There is no footer.

**Rationale for 4096-byte header:**  4096 is one filesystem page on virtually all modern operating systems.  Aligning the log to a page boundary means the OS page cache never has a page that spans both header and log content, which simplifies reasoning about partial writes.  The header only uses a fraction of those 4096 bytes; the remainder is reserved and zero-filled, giving room for future fields without changing the log start offset.

---

## 4. File Header (bytes 0–4095)

All reserved fields must be written as zero and must be ignored on read (future versions may assign meaning to them).

```
Offset  Size  Type      Field
──────  ────  ────────  ─────────────────────────────────────
     0     8  u8[8]     magic
     8     4  u32       format_version
    12     4  u32       page_size
    16     8  u64       create_time_us
    24     8  u64       next_msg_id
    32     8  u64       log_start_offset
    40     8  u64       log_end_offset
    48     4  u32       flags
    52     4  u32       header_crc32
    56  4040  u8[4040]  reserved (must be zero)
```

### 4.1 `magic` (8 bytes)

Exact bytes: `51 44 42 0D 0A 1A 0A 00`

Decoded:
- `QDB` — human-readable identifier
- `0D 0A` — CRLF — detects CRLF translation (file opened in text mode on Windows)
- `1A` — Ctrl-Z — stops `type` command output on DOS/Windows
- `0A` — LF — detects LF-stripping
- `00` — null terminator — detects null-stripping

**Rationale:**  This magic sequence is directly modelled on the PNG magic number (`89 50 4E 47 0D 0A 1A 0A`), which was designed to detect the most common forms of file corruption introduced by text-mode transfers.  The first three bytes spell "QDB" in ASCII for easy identification with `file(1)` or a hex editor.

### 4.2 `format_version` (4 bytes, u32)

The version of this file format specification.  The current version is `1`.

On open:
- If `format_version > QDB_MAX_SUPPORTED_VERSION`, return `QDB_ERR_CORRUPT` (the file was written by a newer library and may use unknown record types).
- If `format_version < 1`, return `QDB_ERR_CORRUPT` (invalid).

### 4.3 `page_size` (4 bytes, u32)

Always `4096` in format version 1.  Reserved for future format versions that may use a different header size.  Implementations must validate that this field equals `4096`; if not, return `QDB_ERR_CORRUPT`.

### 4.4 `create_time_us` (8 bytes, u64)

Unix timestamp in microseconds when the database was created.  Informational only; never used for correctness decisions.

### 4.5 `next_msg_id` (8 bytes, u64)

The next message ID to assign.  When a message is pushed, this value is used as the new message's ID and then incremented by 1.  The updated value is written to the header as part of the push transaction.

Message IDs start at `1`.  ID `0` is reserved and must never appear in a `MSG_PUSH` record.

**Rationale for persisting next_msg_id in the header:**  An alternative is to derive the next ID from the highest ID seen during log scan at startup.  That approach works but requires a full scan before the first push can be served.  Storing the counter in the header allows the first push to proceed immediately after header validation.

### 4.6 `log_start_offset` (8 bytes, u64)

The byte offset of the first valid log record.  After a fresh create, this is `4096`.  After compaction, this advances to point at the first record of the compacted file (still `4096` in the rewritten file, since the header is always rewritten during compaction).

This field exists to make it unambiguous where the log begins, even after future format changes.

### 4.7 `log_end_offset` (8 bytes, u64)

The byte offset one past the last committed record.  On a healthy file this equals the file size.  If the file is longer than `log_end_offset` (e.g. after a crash mid-write), the bytes from `log_end_offset` to EOF are treated as a partial write and ignored.

**Rationale:**  Storing the end offset in the header means recovery does not have to scan forward looking for a truncation boundary — the header tells us exactly where committed data ends.  The trade-off is that every commit must update this field, which adds one `pwrite` + `fsync` per commit.  This cost is acceptable given the reliability-first mandate.

**Alternative considered — commit sentinel only (no end offset in header):**  Each record carries a commit marker byte.  Recovery scans forward until it finds a record whose commit marker is absent, then truncates there.  This works but requires a forward scan of the entire log on every open, even on a healthy file.  Storing `log_end_offset` reduces that to an O(1) seek.

### 4.8 `flags` (4 bytes, u32)

Bitmask of file-level flags.  Currently defined:

| Bit | Name | Meaning |
|---|---|---|
| 0 | `QDB_FLAG_WAL_PRESENT` | A WAL file exists and has not been fully checkpointed |
| 1 | `QDB_FLAG_DIRTY` | The file was not cleanly closed (set on open, cleared on close) |
| 2–31 | reserved | Must be zero; must be ignored on read |

`QDB_FLAG_DIRTY`: set to 1 immediately when the database is opened (before any writes), cleared to 0 on clean close.  If this flag is set when `qdb_open()` reads the header, the previous session did not close cleanly, and WAL recovery must proceed regardless of whether the WAL file exists (it may have been deleted by the OS after a hard power loss).

### 4.9 `header_crc32` (4 bytes, u32)

CRC32 of bytes `0–51` of the header (the 52 bytes preceding this field).  Computed using the standard CRC-32/ISO-HDLC polynomial (`0xEDB88320`).

If the CRC does not match, `qdb_open()` returns `QDB_ERR_CORRUPT` and does not attempt recovery.  A corrupt header means the database identity itself is untrustworthy.

**Rationale:**  The header contains fields like `next_msg_id` and `log_end_offset` that are used to make correctness decisions.  A bit-flip in these fields without detection could cause data loss or incorrect recovery.  A CRC on the header catches the most common hardware-level corruption.

---

## 5. Log Record Wire Format

Every record in the log — regardless of type — shares the same framing:

```
Offset  Size  Type    Field
──────  ────  ──────  ────────────────────────────────────
     0     1  u8      record_type
     1     4  u32     payload_length   (length of payload field only)
     5     4  u32     payload_crc32    (CRC32 of payload field only)
     9     N  u8[N]   payload          (N = payload_length)
   9+N     1  u8      commit_marker    (must be 0xAB when committed)
```

Total record size: `9 + payload_length + 1` bytes.

### 5.1 `record_type` (u8)

| Value | Name | Description |
|---|---|---|
| `0x01` | `RT_MSG_PUSH` | A new message appended to a queue |
| `0x02` | `RT_MSG_LEASE` | A message leased to a consumer |
| `0x03` | `RT_MSG_ACK` | A leased message acknowledged |
| `0x04` | `RT_MSG_NACK` | A leased message rejected; returned to queue |
| `0x05` | `RT_MSG_EXPIRE` | A lease that expired; message returned to queue |
| `0x10` | `RT_CHECKPOINT` | Metadata written after a WAL checkpoint |
| `0xFF` | `RT_PADDING` | Padding/no-op; ignored during scan |

Values `0x06–0x0F`, `0x11–0xFE` are reserved.  An unknown record type encountered during normal operation causes the scan to stop and return `QDB_ERR_CORRUPT` (the file may have been written by a newer format version).

**Rationale for a single record type byte:**  A single byte is sufficient for 255 types, leaves room for decades of extension, and adds only one byte of overhead per record.

### 5.2 `commit_marker` (u8)

The final byte of every record.  When a record is fully and durably written, this byte is set to `0xAB`.

**Write protocol:**
1. Write everything from `record_type` through the last byte of `payload`.  At this point the commit marker byte is not yet written (or is written as `0x00`).
2. Call `fsync`.
3. Write the commit marker byte (`0xAB`) to the correct file offset.
4. Call `fsync`.
5. Update `log_end_offset` in the file header.
6. Call `fsync`.

**Recovery protocol:**  Scan forward from `log_start_offset`.  For each record, if `commit_marker != 0xAB`, the record is incomplete.  Truncate the file at that record's start offset and stop scanning.

**Rationale for trailing commit marker:**  This is the "write ordering" technique used by many database systems (ext3 journaling, PostgreSQL WAL, etc.).  The key property is that the OS cannot reorder writes such that the commit marker appears before the payload — the payload is always written first because it is written as a single `pwrite`.  A process killed between the payload write and the marker write leaves an incomplete record that is detected and discarded.

**Alternative considered — length-prefixed framing only:**  Write `record_type + payload_length + payload`, then derive record boundaries from lengths during scan.  A partial write of the last record would leave a length field whose value points past the end of the file; the scan detects this as truncation.  This works but does not catch cases where the OS writes only part of the payload (partial `pwrite`) yet fills the bytes beyond with zeros — such a record would appear to have a valid length but a bad CRC.  The commit marker catches this because it would be `0x00` (not `0xAB`) even if the payload bytes happened to be consistent.

**Why `0xAB`:**  Not a value that appears in length or type fields, not zero, not a printable ASCII character.  Arbitrary, but documented to prevent accidental byte-value confusion.

### 5.3 `payload_crc32` (u32)

CRC32 of the `payload` bytes only.  Computed using the same polynomial as the header CRC.

If the CRC does not match during a scan, the record is treated as corrupt.  A corrupt record mid-log is a hard error (`QDB_ERR_CORRUPT`); it is not silently skipped, because a CRC failure means the byte stream is not what was written and subsequent records cannot be trusted.

**Alternative considered — per-field checksums:**  More granular corruption detection.  Rejected for complexity; a per-record CRC is the industry standard (LevelDB, RocksDB, PostgreSQL WAL all do this).

---

## 6. Payload Layouts by Record Type

### 6.1 `RT_MSG_PUSH`

```
Offset  Size  Type    Field
──────  ────  ──────  ─────────────────────────────
     0     8  u64     msg_id
     8     1  u8      queue_name_len    (bytes, not chars; 1..255)
     9     N  u8[N]   queue_name        (not NUL-terminated)
   9+N     M  u8[M]   data              (M = plen − 9 − N; may be 0)
```

Total payload size: `9 + queue_name_len + data_len` bytes.

`data_len` is not stored explicitly; it is derived from the record's `payload_length` field:
`data_len = payload_length − 9 − queue_name_len`.

Constraints:
- `msg_id` must be non-zero and equal to the `next_msg_id` value that was in the header when this push began.
- `queue_name_len` must be in [1, 255].
- `data_len` must be in [0, 67108864] (0 bytes to 64 MiB).  Zero-length payloads are valid.

### 6.2 `RT_MSG_LEASE`

```
Offset  Size  Type    Field
──────  ────  ──────  ───────────────────────────────
     0     8  u64     msg_id
     8     8  u64     lease_expiry_us   (unix, microseconds)
    16     8  u64     lease_id          (monotonic, per-database)
```

`lease_id` is a secondary monotonic counter (distinct from `msg_id`) used to distinguish redeliveries.  If a message is leased, expires, and is leased again, the second `RT_MSG_LEASE` record has a higher `lease_id`.  This allows the implementation to detect stale ACKs (an ACK carrying an old `lease_id` for a message that has since been re-leased).

### 6.3 `RT_MSG_ACK`

```
Offset  Size  Type    Field
──────  ────  ──────  ────────────────
     0     8  u64     msg_id
     8     8  u64     lease_id
```

### 6.4 `RT_MSG_NACK`

```
Offset  Size  Type    Field
──────  ────  ──────  ────────────────
     0     8  u64     msg_id
     8     8  u64     lease_id
```

### 6.5 `RT_MSG_EXPIRE`

```
Offset  Size  Type    Field
──────  ────  ──────  ────────────────
     0     8  u64     msg_id
     8     8  u64     lease_id
```

**Rationale for writing `RT_MSG_EXPIRE` as an explicit record:**  Lease expiry could be handled purely in memory (the in-memory index moves the message back to READY when the lease timer fires).  Writing an explicit expiry record makes the log self-describing: a human or tool reading the log can see the full history of every message without needing to reconstruct timer state.  It also simplifies recovery — there is no need to evaluate timer state when replaying the log.

### 6.6 `RT_CHECKPOINT`

```
Offset  Size  Type    Field
──────  ────  ──────  ──────────────────────────────────
     0     8  u64     checkpoint_time_us
     8     8  u64     next_msg_id_at_checkpoint
    16     8  u64     next_lease_id_at_checkpoint
```

Written to the main log after the WAL has been fully applied and the WAL file deleted.  Allows future recovery to establish monotonic counter values without replaying the entire log.

### 6.7 `RT_PADDING`

Payload may be any bytes.  Always ignored.  Used by the compaction process to align records to page boundaries if needed in a future format version.

---

## 7. WAL File Format

The WAL file (`<path>-wal`) uses the same record framing as the main log.  It contains a prefix header followed by a sequence of WAL records.

### 7.1 WAL File Header (64 bytes)

```
Offset  Size  Type    Field
──────  ────  ──────  ─────────────────────────────────────
     0     8  u8[8]   magic             ("QDBWAL\0\0")
     4     4  u32     format_version    (must equal main file version)
     8     8  u64     main_file_log_end_offset  (snapshot of log_end_offset)
    20     4  u32     wal_crc32
    24    40  u8[40]  reserved
```

The WAL magic is distinct from the main file magic to prevent accidentally opening a WAL as a main file.

`main_file_log_end_offset` records where the main file's log ended when this WAL session began.  During recovery, this value tells the implementation exactly where to start applying WAL records into the main file.

### 7.2 WAL Record Types

The WAL carries the same record types as the main log (`RT_MSG_PUSH`, `RT_MSG_LEASE`, etc.).  A WAL record is semantically identical to the corresponding main-log record; it is simply staged in the WAL first.

The WAL is not a delta log — it does not store "patches" to the main file.  It stores the complete record that will be appended to the main file during the next checkpoint.  This makes the checkpoint operation a simple append.

**Rationale:**  A delta-based WAL (storing before/after images of individual bytes) is more complex and is appropriate for in-place update databases (like InnoDB's redo log).  QDB's main file is append-only; the WAL records and the main-log records are structurally identical.  There is nothing to "undo" — only records to append.  The WAL simplifies the durability model: write-to-WAL is the commit; checkpoint-to-main is the cleanup.

---

## 8. Lock File

`<path>-lock` is created with exclusive file locking (`flock(LOCK_EX | LOCK_NB)` on POSIX, `LockFileEx` with `LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY` on Windows) at the start of `qdb_open()` and released at `qdb_close()`.

The file itself has no defined contents.  Its purpose is to prevent two processes from opening the same database simultaneously, which would corrupt the monotonic counter and interleave log records.

If the lock cannot be acquired, `qdb_open()` returns `QDB_ERR_LOCKED`.

**Rationale for a separate lock file rather than locking the main file:**  Locking the main file itself works on POSIX but is unreliable on Windows (where an exclusive lock on the file prevents even read-only opens of the same file by the locking process itself, complicating the implementation).  A separate lock file is consistent across platforms.

---

## 9. File Format Version Compatibility

| Scenario | Behaviour |
|---|---|
| `format_version` == library's version | Normal operation |
| `format_version` > library's version | Return `QDB_ERR_CORRUPT` (too new) |
| `format_version` < library's version | Attempt to upgrade; document in CHANGELOG |

QDB 1.x libraries must be able to read and write all format_version 1.x databases.  A format_version bump to 2 is a breaking change requiring a migration utility.

---

## 10. Checksums

QDB uses CRC-32/ISO-HDLC (also known as CRC-32b) throughout.

Polynomial: `0xEDB88320` (reflected)
Initial value: `0xFFFFFFFF`
Final XOR: `0xFFFFFFFF`

This is the same CRC used by zlib, PNG, and Ethernet.  Implementations may use `zlib`'s `crc32()` function or any compatible implementation.  A reference implementation will be included in `src/crc32.c`.

**Rationale:**  CRC-32 provides good single-bit error detection, is cheap to compute (hardware acceleration via `crc32` instructions on x86 and ARM), and is universally understood.  It is not a cryptographic hash and does not protect against intentional tampering — QDB does not have that threat model in v1.  For v1's threat model (hardware bit-flips, power-loss mid-write), CRC-32 is sufficient.

**Alternative considered — CRC-64:**  Better Hamming distance for longer payloads.  Rejected because the 64 MiB payload limit means payloads are short enough that CRC-32 is statistically adequate, and CRC-64 lacks the same ubiquity of hardware acceleration.

**Alternative considered — xxHash:**  Faster than CRC-32.  Rejected because QDB's I/O cost dominates checksum computation cost; the choice of hash algorithm has no measurable impact on throughput.
