/*
 * qdb_io.c — record-level I/O primitives
 *
 * Implements the append-only log write protocol, record scanning, and
 * file header encode/decode.  All functions are documented in
 * qdb_internal.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb_internal.h"

/* -------------------------------------------------------------------------
 * Magic byte constants
 * ---------------------------------------------------------------------- */

/* "QDB\r\n\x1a\n\0" — modelled on PNG magic */
static const uint8_t k_file_magic[8] = {
    0x51u, 0x44u, 0x42u, 0x0Du, 0x0Au, 0x1Au, 0x0Au, 0x00u
};

/* -------------------------------------------------------------------------
 * Low-level read / write wrappers
 * ---------------------------------------------------------------------- */

int qdb__read_full(qdb__fd_t fd, void *buf, size_t len, uint64_t offset)
{
    size_t nread = 0;
    int rc = qdb__pread(fd, buf, len, offset, &nread);
    if (rc != QDB_OK) {
        return rc;
    }
    return (nread == len) ? QDB_OK : QDB_ERR_IO;
}

int qdb__write_full(qdb__fd_t fd, const void *buf, size_t len, uint64_t offset)
{
    return qdb__pwrite(fd, buf, len, offset);
}

/* -------------------------------------------------------------------------
 * Header encode / decode
 *
 * The header is exactly QDB_HDR_SIZE (4096) bytes.  We encode/decode from
 * a flat byte array to avoid any struct-layout assumptions.
 * ---------------------------------------------------------------------- */

static void header_encode(uint8_t buf[QDB_HDR_SIZE], const struct qdb *db)
{
    uint32_t crc;

    memset(buf, 0, QDB_HDR_SIZE);

    memcpy(buf + QDB_HDR_OFF_MAGIC, k_file_magic, 8);
    qdb__put_u32le(buf + QDB_HDR_OFF_VERSION,      QDB_FORMAT_VERSION);
    qdb__put_u32le(buf + QDB_HDR_OFF_PAGE_SIZE,     QDB_PAGE_SIZE);
    qdb__put_u64le(buf + QDB_HDR_OFF_CREATE_TIME,   db->create_time_us);
    qdb__put_u64le(buf + QDB_HDR_OFF_NEXT_MSG_ID,   db->next_msg_id);
    qdb__put_u64le(buf + QDB_HDR_OFF_LOG_START,     db->log_start_offset);
    qdb__put_u64le(buf + QDB_HDR_OFF_LOG_END,       db->log_end_offset);
    qdb__put_u32le(buf + QDB_HDR_OFF_FLAGS,         db->flags);

    crc = qdb__crc32(buf, QDB_HDR_CRC_COVER);
    qdb__put_u32le(buf + QDB_HDR_OFF_CRC32, crc);
    /* reserved bytes remain zero */
}

static int header_decode(const uint8_t buf[QDB_HDR_SIZE], struct qdb *db)
{
    uint32_t stored_crc, actual_crc, version, page_size;

    /* Magic */
    if (memcmp(buf + QDB_HDR_OFF_MAGIC, k_file_magic, 8) != 0) {
        return QDB_ERR_CORRUPT;
    }

    /* CRC of bytes [0..51] */
    actual_crc = qdb__crc32(buf, QDB_HDR_CRC_COVER);
    stored_crc = qdb__get_u32le(buf + QDB_HDR_OFF_CRC32);
    if (actual_crc != stored_crc) {
        return QDB_ERR_CORRUPT;
    }

    /* Format version */
    version = qdb__get_u32le(buf + QDB_HDR_OFF_VERSION);
    if (version < 1u || version > QDB_MAX_FORMAT_VERSION) {
        return QDB_ERR_CORRUPT;
    }

    /* Page size */
    page_size = qdb__get_u32le(buf + QDB_HDR_OFF_PAGE_SIZE);
    if (page_size != QDB_PAGE_SIZE) {
        return QDB_ERR_CORRUPT;
    }

    db->create_time_us    = qdb__get_u64le(buf + QDB_HDR_OFF_CREATE_TIME);
    db->next_msg_id       = qdb__get_u64le(buf + QDB_HDR_OFF_NEXT_MSG_ID);
    db->log_start_offset  = qdb__get_u64le(buf + QDB_HDR_OFF_LOG_START);
    db->log_end_offset    = qdb__get_u64le(buf + QDB_HDR_OFF_LOG_END);
    db->flags             = qdb__get_u32le(buf + QDB_HDR_OFF_FLAGS);

    /* Sanity: offsets must be at least QDB_HDR_SIZE */
    if (db->log_start_offset < QDB_HDR_SIZE ||
        db->log_end_offset   < db->log_start_offset) {
        return QDB_ERR_CORRUPT;
    }

    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * Public header read / write
 * ---------------------------------------------------------------------- */

int qdb__header_write(qdb__fd_t fd, const struct qdb *db)
{
    uint8_t buf[QDB_HDR_SIZE];

    header_encode(buf, db);
    if (qdb__write_full(fd, buf, QDB_HDR_SIZE, 0) != QDB_OK) {
        return QDB_ERR_IO;
    }
    return qdb__file_sync(fd);
}

int qdb__header_read(qdb__fd_t fd, struct qdb *db)
{
    uint8_t buf[QDB_HDR_SIZE];
    uint64_t file_sz = 0;

    /* The file must be at least a full header. */
    if (qdb__file_size(fd, &file_sz) != QDB_OK) {
        return QDB_ERR_IO;
    }
    if (file_sz < QDB_HDR_SIZE) {
        return QDB_ERR_CORRUPT;
    }

    if (qdb__read_full(fd, buf, QDB_HDR_SIZE, 0) != QDB_OK) {
        return QDB_ERR_IO;
    }

    return header_decode(buf, db);
}

int qdb__header_update(qdb__fd_t fd, const struct qdb *db)
{
    /* Build the 56-byte prefix in a stack buffer, compute CRC, then
     * write only bytes [40..55] (log_end + flags + crc32). */
    uint8_t  prefix[QDB_HDR_CRC_COVER + 4u]; /* bytes 0..55 */
    uint32_t crc;
    uint8_t  patch[16]; /* log_end(8) + flags(4) + crc(4) */

    memcpy(prefix + QDB_HDR_OFF_MAGIC,       k_file_magic, 8);
    qdb__put_u32le(prefix + QDB_HDR_OFF_VERSION,      QDB_FORMAT_VERSION);
    qdb__put_u32le(prefix + QDB_HDR_OFF_PAGE_SIZE,    QDB_PAGE_SIZE);
    qdb__put_u64le(prefix + QDB_HDR_OFF_CREATE_TIME,  db->create_time_us);
    qdb__put_u64le(prefix + QDB_HDR_OFF_NEXT_MSG_ID,  db->next_msg_id);
    qdb__put_u64le(prefix + QDB_HDR_OFF_LOG_START,    db->log_start_offset);
    qdb__put_u64le(prefix + QDB_HDR_OFF_LOG_END,      db->log_end_offset);
    qdb__put_u32le(prefix + QDB_HDR_OFF_FLAGS,        db->flags);

    crc = qdb__crc32(prefix, QDB_HDR_CRC_COVER);

    /* patch = bytes [40..55]: log_end(8), flags(4), crc32(4) */
    qdb__put_u64le(patch + 0, db->log_end_offset);
    qdb__put_u32le(patch + 8, db->flags);
    qdb__put_u32le(patch + 12, crc);

    if (qdb__write_full(fd, patch, sizeof(patch), QDB_HDR_OFF_LOG_END) != QDB_OK) {
        return QDB_ERR_IO;
    }
    return qdb__file_sync(fd);
}

/* -------------------------------------------------------------------------
 * Append one record — full durability write protocol
 * ---------------------------------------------------------------------- */

int qdb__append_record(qdb__fd_t fd, uint8_t type,
                       const void *payload, uint32_t plen,
                       uint64_t *offset)
{
    uint8_t  hdr[QDB_REC_HDR_SIZE];
    uint8_t  marker = QDB_COMMIT_MARKER;
    uint32_t crc;
    uint64_t hdr_off    = *offset;
    uint64_t payload_off = hdr_off + QDB_REC_HDR_SIZE;
    uint64_t marker_off  = payload_off + (uint64_t)plen;

    crc = qdb__crc32(payload, (size_t)plen);

    hdr[QDB_REC_OFF_TYPE] = type;
    qdb__put_u32le(hdr + QDB_REC_OFF_PLEN, plen);
    qdb__put_u32le(hdr + QDB_REC_OFF_CRC,  crc);

    /* Step 1: write header + payload (no commit marker yet). */
    if (qdb__write_full(fd, hdr, QDB_REC_HDR_SIZE, hdr_off) != QDB_OK) {
        return QDB_ERR_IO;
    }
    if (plen > 0) {
        if (qdb__write_full(fd, payload, (size_t)plen, payload_off) != QDB_OK) {
            return QDB_ERR_IO;
        }
    }

    /* Step 2: fsync — ensures payload is durable before marker. */
    if (qdb__file_sync(fd) != QDB_OK) {
        return QDB_ERR_IO;
    }

    /* Step 3: write commit marker. */
    if (qdb__write_full(fd, &marker, 1, marker_off) != QDB_OK) {
        return QDB_ERR_IO;
    }

    /* Step 4: fsync — record is now fully committed. */
    if (qdb__file_sync(fd) != QDB_OK) {
        return QDB_ERR_IO;
    }

    *offset = marker_off + 1u;
    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * Scan one record — validate without returning payload bytes to caller
 * ---------------------------------------------------------------------- */

static int is_known_type(uint8_t type)
{
    switch (type) {
    case QDB_RT_MSG_PUSH:
    case QDB_RT_MSG_LEASE:
    case QDB_RT_MSG_ACK:
    case QDB_RT_MSG_NACK:
    case QDB_RT_MSG_EXPIRE:
    case QDB_RT_CHECKPOINT:
    case QDB_RT_PADDING:
        return 1;
    default:
        return 0;
    }
}

int qdb__scan_record(qdb__fd_t fd, uint64_t *offset, uint64_t end_offset,
                     uint8_t *out_type, uint32_t *out_plen)
{
    uint8_t  hdr[QDB_REC_HDR_SIZE];
    uint8_t  marker;
    uint8_t  chunk[4096];
    uint32_t stored_crc, plen;
    uint64_t payload_off, marker_off, record_end;
    uint32_t crc_state;
    uint32_t remaining;

    if (*offset >= end_offset) {
        return QDB__SCAN_END;
    }

    /* Need at least a full record header + marker. */
    if (end_offset - *offset < (uint64_t)(QDB_REC_HDR_SIZE + 1u)) {
        /* Fewer bytes than the minimum record size: treat as partial. */
        return QDB__SCAN_PARTIAL;
    }

    if (qdb__read_full(fd, hdr, QDB_REC_HDR_SIZE, *offset) != QDB_OK) {
        return QDB_ERR_IO;
    }

    plen       = qdb__get_u32le(hdr + QDB_REC_OFF_PLEN);
    stored_crc = qdb__get_u32le(hdr + QDB_REC_OFF_CRC);

    /* Guard against absurdly large payloads before computing addresses. */
    if (plen > QDB_MAX_RECORD_PAYLOAD) {
        return QDB_ERR_CORRUPT;
    }

    payload_off = *offset + QDB_REC_HDR_SIZE;
    marker_off  = payload_off + (uint64_t)plen;
    record_end  = marker_off + 1u;

    /* Record must fit within end_offset. */
    if (record_end > end_offset) {
        return QDB__SCAN_PARTIAL;
    }

    /* Read commit marker. */
    if (qdb__read_full(fd, &marker, 1, marker_off) != QDB_OK) {
        return QDB_ERR_IO;
    }
    if (marker != QDB_COMMIT_MARKER) {
        return QDB__SCAN_PARTIAL;
    }

    /* Validate record type (after confirming the record is committed). */
    if (!is_known_type(hdr[QDB_REC_OFF_TYPE])) {
        return QDB_ERR_CORRUPT;
    }

    /* CRC check: read payload in 4 KB chunks to avoid large allocations. */
    crc_state = qdb__crc32_begin();
    remaining = plen;

    while (remaining > 0) {
        uint32_t to_read = (remaining < (uint32_t)sizeof(chunk))
                           ? remaining
                           : (uint32_t)sizeof(chunk);
        uint64_t chunk_off = payload_off + ((uint64_t)(plen - remaining));

        if (qdb__read_full(fd, chunk, (size_t)to_read, chunk_off) != QDB_OK) {
            return QDB_ERR_IO;
        }
        crc_state  = qdb__crc32_update(crc_state, chunk, (size_t)to_read);
        remaining -= to_read;
    }

    if (qdb__crc32_end(crc_state) != stored_crc) {
        return QDB_ERR_CORRUPT;
    }

    *out_type  = hdr[QDB_REC_OFF_TYPE];
    *out_plen  = plen;
    *offset    = record_end;
    return QDB_OK;
}

int qdb__read_payload(qdb__fd_t fd, uint64_t payload_offset,
                      void *buf, uint32_t plen)
{
    return qdb__read_full(fd, buf, (size_t)plen, payload_offset);
}
