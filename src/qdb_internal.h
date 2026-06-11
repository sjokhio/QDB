/*
 * qdb_internal.h — internal types, constants, and declarations
 *
 * Not installed.  Must not be included by application code.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QDB_INTERNAL_H
#define QDB_INTERNAL_H

#include "qdb.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Platform file-descriptor type
 *
 * intptr_t holds a POSIX int fd (trivially) and a Windows HANDLE (which
 * is a pointer, always fitting in intptr_t on every supported ABI).
 * INVALID_HANDLE_VALUE on Windows is (HANDLE)(uintptr_t)-1, which cast to
 * intptr_t is -1 — the same sentinel we use on POSIX.
 * ---------------------------------------------------------------------- */

typedef intptr_t qdb__fd_t;
#define QDB__INVALID_FD ((qdb__fd_t)(-1))

/* -------------------------------------------------------------------------
 * Internal-only scan return codes (positive, distinct from QDB_OK = 0)
 * ---------------------------------------------------------------------- */

#define QDB__SCAN_END     1   /* reached end_offset — no more records    */
#define QDB__SCAN_PARTIAL 2   /* uncommitted tail record — caller truncates */

/* -------------------------------------------------------------------------
 * File format constants
 * ---------------------------------------------------------------------- */

#define QDB_FORMAT_VERSION      1u
#define QDB_MAX_FORMAT_VERSION  1u
#define QDB_PAGE_SIZE        4096u

/* File header field offsets (bytes within the 4096-byte header) */
#define QDB_HDR_OFF_MAGIC        0u   /* u8[8]  */
#define QDB_HDR_OFF_VERSION      8u   /* u32    */
#define QDB_HDR_OFF_PAGE_SIZE   12u   /* u32    */
#define QDB_HDR_OFF_CREATE_TIME 16u   /* u64    */
#define QDB_HDR_OFF_NEXT_MSG_ID 24u   /* u64    */
#define QDB_HDR_OFF_LOG_START   32u   /* u64    */
#define QDB_HDR_OFF_LOG_END     40u   /* u64    */
#define QDB_HDR_OFF_FLAGS       48u   /* u32    */
#define QDB_HDR_OFF_CRC32       52u   /* u32    — CRC32 of bytes [0..51] */
#define QDB_HDR_OFF_RESERVED    56u   /* u8[4040] */
#define QDB_HDR_CRC_COVER       52u   /* byte count covered by header CRC */
#define QDB_HDR_SIZE          4096u

/* File-level flags (stored in header flags field) */
#define QDB_FLAG_WAL_PRESENT   (1u << 0)
#define QDB_FLAG_DIRTY         (1u << 1)

/* Record type bytes */
#define QDB_RT_MSG_PUSH    ((uint8_t)0x01u)
#define QDB_RT_MSG_LEASE   ((uint8_t)0x02u)
#define QDB_RT_MSG_ACK     ((uint8_t)0x03u)
#define QDB_RT_MSG_NACK    ((uint8_t)0x04u)
#define QDB_RT_MSG_EXPIRE  ((uint8_t)0x05u)
#define QDB_RT_CHECKPOINT  ((uint8_t)0x10u)
#define QDB_RT_PADDING     ((uint8_t)0xFFu)

/* Record framing */
#define QDB_REC_OFF_TYPE     0u   /* u8  */
#define QDB_REC_OFF_PLEN     1u   /* u32 */
#define QDB_REC_OFF_CRC      5u   /* u32 */
#define QDB_REC_OFF_PAYLOAD  9u   /* u8[N] */
#define QDB_REC_HDR_SIZE     9u   /* type(1) + plen(4) + crc(4) */
#define QDB_COMMIT_MARKER  ((uint8_t)0xABu)

/* WAL header field offsets within the 64-byte WAL header.
 * Note: the design doc lists format_version at offset 4, which conflicts
 * with the 8-byte magic.  The correct layout has format_version at 8. */
#define QDB_WAL_OFF_MAGIC    0u   /* u8[8]  */
#define QDB_WAL_OFF_VERSION  8u   /* u32    */
#define QDB_WAL_OFF_LOG_END 12u   /* u64    — main file log_end at WAL creation */
#define QDB_WAL_OFF_CRC32   20u   /* u32    */
#define QDB_WAL_OFF_RSVD    24u   /* u8[40] */
#define QDB_WAL_HDR_SIZE    64u
#define QDB_WAL_HDR_CRC_COVER 20u /* bytes [0..19] covered by WAL CRC */

/* Fixed payload sizes for non-push record types */
#define QDB_PAYLOAD_LEASE_SIZE      24u  /* msg_id(8)+expiry(8)+lease_id(8) */
#define QDB_PAYLOAD_ACK_SIZE        16u  /* msg_id(8)+lease_id(8) */
#define QDB_PAYLOAD_NACK_SIZE       16u
#define QDB_PAYLOAD_EXPIRE_SIZE     16u
#define QDB_PAYLOAD_CHECKPOINT_SIZE 24u  /* time(8)+next_msg_id(8)+next_lease_id(8) */

/* Maximum sensible payload length (64 MiB + max fixed push overhead) */
#define QDB_MAX_RECORD_PAYLOAD (QDB_MSG_MAX_LEN + 512u)

/* RT_MSG_PUSH payload field offsets (variable length):
 *   offset 0: u64  msg_id
 *   offset 8: u8   queue_name_len  (1..QDB_QUEUE_NAME_MAX)
 *   offset 9: u8[] queue_name
 *   offset 9+queue_name_len: u8[]  message_data
 */
#define QDB_PUSH_OFF_MSG_ID    0u   /* u64 */
#define QDB_PUSH_OFF_QNAME_LEN 8u   /* u8  */
#define QDB_PUSH_OFF_QNAME     9u   /* u8[queue_name_len] */
#define QDB_PUSH_HDR_SIZE      9u   /* msg_id(8) + name_len(1) */
#define QDB_PUSH_MIN_PLEN     10u   /* msg_id + name_len=1 + 1-char name */

/* RT_MSG_LEASE payload field offsets (QDB_PAYLOAD_LEASE_SIZE = 24 bytes) */
#define QDB_LEASE_OFF_MSG_ID    0u   /* u64 */
#define QDB_LEASE_OFF_EXPIRY    8u   /* u64 microseconds since epoch */
#define QDB_LEASE_OFF_LEASE_ID 16u   /* u64 */

/* RT_MSG_ACK / RT_MSG_NACK / RT_MSG_EXPIRE payload field offsets
 * (QDB_PAYLOAD_ACK_SIZE = 16 bytes) */
#define QDB_ACK_OFF_MSG_ID   0u     /* u64 */
#define QDB_ACK_OFF_LEASE_ID 8u     /* u64 */

/* RT_CHECKPOINT payload field offsets (QDB_PAYLOAD_CHECKPOINT_SIZE = 24 bytes) */
#define QDB_CKPT_OFF_TIME_US        0u  /* u64 */
#define QDB_CKPT_OFF_NEXT_MSG_ID    8u  /* u64 */
#define QDB_CKPT_OFF_NEXT_LEASE_ID 16u  /* u64 */

/* -------------------------------------------------------------------------
 * Internal database handle
 * ---------------------------------------------------------------------- */

struct qdb__state;  /* defined in qdb_state.h */

struct qdb {
    qdb__fd_t   fd;           /* main file                   */
    qdb__fd_t   lock_fd;      /* lock file                   */

    char       *path;         /* owned copy of db path       */
    char       *wal_path;     /* path + "-wal"  (owned)      */
    char       *lock_path;    /* path + "-lock" (owned)      */

    /* Mirrored header fields */
    uint64_t    next_msg_id;
    uint64_t    next_lease_id;
    uint64_t    log_start_offset;
    uint64_t    log_end_offset;
    uint64_t    create_time_us;
    uint32_t    flags;

    /* In-memory queue state; NULL until qdb__replay_log() completes */
    struct qdb__state *state;
};

/* -------------------------------------------------------------------------
 * Little-endian encode/decode — no unaligned pointer casts, no UB
 * ---------------------------------------------------------------------- */

static inline uint16_t qdb__get_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static inline uint32_t qdb__get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8u)
         | ((uint32_t)p[2] << 16u)
         | ((uint32_t)p[3] << 24u);
}

static inline uint64_t qdb__get_u64le(const uint8_t *p)
{
    return (uint64_t)qdb__get_u32le(p)
         | ((uint64_t)qdb__get_u32le(p + 4) << 32u);
}

static inline void qdb__put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
}

static inline void qdb__put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >>  8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static inline void qdb__put_u64le(uint8_t *p, uint64_t v)
{
    qdb__put_u32le(p,     (uint32_t)(v & 0xFFFFFFFFu));
    qdb__put_u32le(p + 4, (uint32_t)(v >> 32u));
}

/* -------------------------------------------------------------------------
 * CRC-32/ISO-HDLC  (qdb_crc32.c)
 * ---------------------------------------------------------------------- */

uint32_t qdb__crc32_begin(void);
uint32_t qdb__crc32_update(uint32_t state, const void *data, size_t len);
uint32_t qdb__crc32_end(uint32_t state);
uint32_t qdb__crc32(const void *data, size_t len);

/* -------------------------------------------------------------------------
 * Platform I/O  (qdb_platform.c)
 * ---------------------------------------------------------------------- */

/*
 * Open a file for read/write.  If the file does not exist and create=1,
 * it is created with mode 0600.  *out_is_new is set to 1 if the file was
 * just created, 0 if it already existed.
 */
int qdb__file_open(const char *path, int create,
                   qdb__fd_t *out_fd, int *out_is_new);

/* Open or create the lock file (write access not required). */
int qdb__lockfile_open(const char *path, qdb__fd_t *out_fd);

void qdb__file_close(qdb__fd_t fd);

/* Platform fsync: fdatasync on Linux, F_FULLFSYNC on macOS, FlushFileBuffers on Windows. */
int qdb__file_sync(qdb__fd_t fd);

/* Return the current size of fd in bytes. */
int qdb__file_size(qdb__fd_t fd, uint64_t *out_size);

/* Truncate fd to exactly size bytes. */
int qdb__file_truncate(qdb__fd_t fd, uint64_t size);

/*
 * Positional read.  Reads up to len bytes; *out_nread is the actual count.
 * A short read (< len) with QDB_OK means EOF was reached.
 */
int qdb__pread(qdb__fd_t fd, void *buf, size_t len,
               uint64_t offset, size_t *out_nread);

/*
 * Positional write.  Writes exactly len bytes; retries on EINTR.
 * Returns QDB_ERR_IO if fewer than len bytes were written.
 */
int qdb__pwrite(qdb__fd_t fd, const void *buf, size_t len, uint64_t offset);

/* Acquire exclusive non-blocking lock.  Returns QDB_ERR_LOCKED if busy. */
int qdb__file_lock(qdb__fd_t fd);

/* Release lock (best-effort; never fails visibly). */
void qdb__file_unlock(qdb__fd_t fd);

/* Delete a file by path.  Returns QDB_OK if deleted or not found. */
int qdb__file_delete(const char *path);

/* Atomic rename (overwrites destination). */
int qdb__file_rename(const char *old_path, const char *new_path);

/* Current wall-clock time in microseconds since the Unix epoch. */
uint64_t qdb__time_us(void);

/* -------------------------------------------------------------------------
 * Record I/O  (qdb_io.c)
 * ---------------------------------------------------------------------- */

/*
 * Read exactly len bytes from offset; returns QDB_ERR_IO on short read.
 */
int qdb__read_full(qdb__fd_t fd, void *buf, size_t len, uint64_t offset);

/*
 * Write exactly len bytes to offset; returns QDB_ERR_IO on any failure.
 */
int qdb__write_full(qdb__fd_t fd, const void *buf, size_t len, uint64_t offset);

/*
 * Append one record to fd using the full durability write protocol:
 *   1. Write record header + payload to *offset.
 *   2. qdb__file_sync(fd).
 *   3. Write commit marker (0xAB).
 *   4. qdb__file_sync(fd).
 *   5. Advance *offset by the total record size.
 *
 * Does NOT update log_end_offset in the file header — caller's responsibility.
 */
int qdb__append_record(qdb__fd_t fd, uint8_t type,
                       const void *payload, uint32_t plen,
                       uint64_t *offset);

/*
 * Read and validate the next record starting at *offset.
 *
 * Returns:
 *   QDB_OK           — record valid; *out_type and *out_plen set; *offset advanced.
 *   QDB__SCAN_END    — *offset == end_offset; nothing to read.
 *   QDB__SCAN_PARTIAL — commit marker absent; *offset NOT advanced (caller truncates here).
 *   QDB_ERR_CORRUPT  — CRC mismatch or unknown record type.
 *   QDB_ERR_IO       — read failure.
 *
 * Payload bytes are consumed from disk to compute the CRC but are not returned.
 * To retrieve the payload, use qdb__read_payload() after this call.
 */
int qdb__scan_record(qdb__fd_t fd, uint64_t *offset, uint64_t end_offset,
                     uint8_t *out_type, uint32_t *out_plen);

/*
 * Read payload bytes of a record whose header was already scanned.
 * payload_offset = record_start + QDB_REC_HDR_SIZE.
 * Caller provides a buffer of at least plen bytes.
 */
int qdb__read_payload(qdb__fd_t fd, uint64_t payload_offset,
                      void *buf, uint32_t plen);

/* Write the complete 4096-byte file header from db state; sync after. */
int qdb__header_write(qdb__fd_t fd, const struct qdb *db);

/* Read and fully validate the file header; populate db fields. */
int qdb__header_read(qdb__fd_t fd, struct qdb *db);

/*
 * Write the mutable header fields (next_msg_id, log_start, log_end, flags,
 * crc32 — bytes 24..55) then sync.  Cheaper than a full 4096-byte write.
 * All other fields are read from db (mirrored there).
 */
int qdb__header_update(qdb__fd_t fd, const struct qdb *db);

/* -------------------------------------------------------------------------
 * Log replay  (qdb_replay.c)
 * ---------------------------------------------------------------------- */

/*
 * Scan every committed record in db's log, reconstruct in-memory queue
 * state, and store the result in db->state.  Updates db->next_msg_id and
 * db->next_lease_id from the record stream.
 *
 * Returns QDB_OK, QDB_ERR_CORRUPT, QDB_ERR_IO, or QDB_ERR_NOMEM.
 * On error db->state is left NULL (caller should treat db as unusable).
 */
int qdb__replay_log(qdb_t *db);

#endif /* QDB_INTERNAL_H */
