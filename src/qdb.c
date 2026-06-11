/*
 * qdb.c — database lifecycle and public API
 *
 * Implements qdb_open() and qdb_close().  Queue operations (push/pop/ack)
 * remain stubs until the WAL write path is implemented.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb_state.h"

#include <stdlib.h>
#include <string.h>

/* WAL magic bytes — defined here to avoid a cross-TU dependency on the
 * file-scope static in qdb_io.c.  Both definitions must match. */
static const uint8_t s_wal_magic[8] = {
    0x51u, 0x44u, 0x42u, 0x57u, 0x41u, 0x4Cu, 0x00u, 0x00u
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void qdb__free_db(qdb_t *db)
{
    if (!db) {
        return;
    }
    qdb__state_free(db->state);
    db->state = NULL;
    if (db->fd != QDB__INVALID_FD) {
        qdb__file_close(db->fd);
    }
    if (db->lock_fd != QDB__INVALID_FD) {
        qdb__file_unlock(db->lock_fd);
        qdb__file_close(db->lock_fd);
    }
    free(db->path);
    free(db->wal_path);
    free(db->lock_path);
    free(db);
}

/* Build "<base><suffix>\0".  Caller frees result.  Returns NULL on OOM. */
static char *make_sidecar(const char *base, size_t base_len, const char *suffix)
{
    size_t suf_len = strlen(suffix);
    char  *out     = (char *)malloc(base_len + suf_len + 1u);
    if (!out) {
        return NULL;
    }
    memcpy(out,            base,   base_len);
    memcpy(out + base_len, suffix, suf_len + 1u);
    return out;
}

/* -------------------------------------------------------------------------
 * WAL replay
 *
 * Scans committed records from the WAL file and appends each to the main
 * file.  In this phase push/pop/ack are stubs so no WAL records will exist
 * in practice; the path is implemented in full so the next phase can
 * simply start writing WAL records without touching this code.
 * ---------------------------------------------------------------------- */

static int replay_wal(qdb_t *db)
{
    qdb__fd_t wal_fd      = QDB__INVALID_FD;
    uint8_t   hdr[QDB_WAL_HDR_SIZE];
    uint64_t  wal_size    = 0;
    uint64_t  wal_offset  = QDB_WAL_HDR_SIZE;
    int       is_new_ignored = 0;
    int       rc;

    rc = qdb__file_open(db->wal_path, 0, &wal_fd, &is_new_ignored);
    if (rc != QDB_OK) {
        return QDB_OK; /* no WAL file — nothing to do */
    }

    rc = qdb__file_size(wal_fd, &wal_size);
    if (rc != QDB_OK || wal_size < QDB_WAL_HDR_SIZE) {
        qdb__file_close(wal_fd);
        (void)qdb__file_delete(db->wal_path);
        return QDB_OK;
    }

    if (qdb__read_full(wal_fd, hdr, QDB_WAL_HDR_SIZE, 0) != QDB_OK) {
        qdb__file_close(wal_fd);
        return QDB_ERR_IO;
    }

    /* Validate WAL header: magic, CRC, version. */
    if (memcmp(hdr + QDB_WAL_OFF_MAGIC, s_wal_magic, 8) != 0) {
        qdb__file_close(wal_fd);
        return QDB_ERR_CORRUPT;
    }
    {
        uint32_t stored = qdb__get_u32le(hdr + QDB_WAL_OFF_CRC32);
        uint32_t actual = qdb__crc32(hdr, QDB_WAL_HDR_CRC_COVER);
        if (stored != actual) {
            qdb__file_close(wal_fd);
            return QDB_ERR_CORRUPT;
        }
    }
    {
        uint32_t ver = qdb__get_u32le(hdr + QDB_WAL_OFF_VERSION);
        if (ver < 1u || ver > QDB_MAX_FORMAT_VERSION) {
            qdb__file_close(wal_fd);
            return QDB_ERR_CORRUPT;
        }
    }

    /* Append each committed WAL record to the main file. */
    while (wal_offset < wal_size) {
        uint8_t  type;
        uint32_t plen;
        uint64_t rec_start = wal_offset;
        void    *payload   = NULL;

        rc = qdb__scan_record(wal_fd, &wal_offset, wal_size, &type, &plen);
        if (rc == QDB__SCAN_END || rc == QDB__SCAN_PARTIAL) {
            break;
        }
        if (rc != QDB_OK) {
            qdb__file_close(wal_fd);
            return rc;
        }

        /* Read this record's payload from the WAL. */
        if (plen > 0) {
            payload = malloc((size_t)plen);
            if (!payload) {
                qdb__file_close(wal_fd);
                return QDB_ERR_NOMEM;
            }
            rc = qdb__read_payload(wal_fd,
                                   rec_start + QDB_REC_HDR_SIZE,
                                   payload, plen);
            if (rc != QDB_OK) {
                free(payload);
                qdb__file_close(wal_fd);
                return rc;
            }
        }

        rc = qdb__append_record(db->fd, type, payload, plen,
                                &db->log_end_offset);
        free(payload);
        if (rc != QDB_OK) {
            qdb__file_close(wal_fd);
            return rc;
        }
    }

    qdb__file_close(wal_fd);

    /* Update header, then remove the WAL. */
    db->flags &= ~QDB_FLAG_WAL_PRESENT;
    if (qdb__header_update(db->fd, db) != QDB_OK) {
        return QDB_ERR_IO;
    }
    (void)qdb__file_delete(db->wal_path);
    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * Log scan — validate records, truncate uncommitted tail
 * ---------------------------------------------------------------------- */

static int validate_log(qdb_t *db)
{
    uint64_t offset      = db->log_start_offset;
    int      need_update = 0;

    while (offset < db->log_end_offset) {
        uint8_t  type;
        uint32_t plen;
        int rc = qdb__scan_record(db->fd, &offset, db->log_end_offset,
                                  &type, &plen);

        if (rc == QDB__SCAN_END) {
            break;
        }
        if (rc == QDB__SCAN_PARTIAL) {
            /* offset is the start of the partial record; truncate there. */
            if (qdb__file_truncate(db->fd, offset) != QDB_OK) {
                return QDB_ERR_IO;
            }
            db->log_end_offset = offset;
            need_update = 1;
            break;
        }
        if (rc != QDB_OK) {
            return rc;
        }
        /* Queue-phase: index rebuild will go here. */
        (void)type;
        (void)plen;
    }

    if (need_update) {
        return qdb__header_update(db->fd, db);
    }
    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * qdb_open
 * ---------------------------------------------------------------------- */

qdb_t *qdb_open(const char *path)
{
    qdb_t  *db;
    size_t  path_len;
    int     is_new = 0;
    int     rc;

    if (!path) {
        return NULL;
    }

    db = (qdb_t *)calloc(1, sizeof(*db));
    if (!db) {
        return NULL;
    }
    db->fd      = QDB__INVALID_FD;
    db->lock_fd = QDB__INVALID_FD;

    path_len      = strlen(path);
    db->path      = make_sidecar(path, path_len, "");
    db->wal_path  = make_sidecar(path, path_len, "-wal");
    db->lock_path = make_sidecar(path, path_len, "-lock");

    if (!db->path || !db->wal_path || !db->lock_path) {
        qdb__free_db(db);
        return NULL;
    }

    /* 1. Acquire exclusive file lock. */
    if (qdb__lockfile_open(db->lock_path, &db->lock_fd) != QDB_OK) {
        qdb__free_db(db);
        return NULL;
    }
    rc = qdb__file_lock(db->lock_fd);
    if (rc != QDB_OK) {
        qdb__file_close(db->lock_fd);
        db->lock_fd = QDB__INVALID_FD;
        qdb__free_db(db);
        return NULL;
    }

    /* 2. Open or create the main file. */
    if (qdb__file_open(db->path, 1, &db->fd, &is_new) != QDB_OK) {
        qdb__free_db(db);
        return NULL;
    }

    if (is_new) {
        /* Fresh database: write initial header and initialise empty state. */
        db->next_msg_id      = 1u;
        db->next_lease_id    = 1u;
        db->log_start_offset = QDB_HDR_SIZE;
        db->log_end_offset   = QDB_HDR_SIZE;
        db->create_time_us   = qdb__time_us();
        db->flags            = QDB_FLAG_DIRTY;

        if (qdb__header_write(db->fd, db) != QDB_OK) {
            qdb__free_db(db);
            return NULL;
        }

        db->state = qdb__state_alloc();
        if (!db->state) {
            qdb__free_db(db);
            return NULL;
        }
        return db;
    }

    /* 3. Validate header. */
    if (qdb__header_read(db->fd, db) != QDB_OK) {
        qdb__free_db(db);
        return NULL;
    }

    /* 4. Replay WAL if present or if dirty flag is set. */
    if ((db->flags & QDB_FLAG_WAL_PRESENT) ||
        (db->flags & QDB_FLAG_DIRTY)) {
        if (replay_wal(db) != QDB_OK) {
            qdb__free_db(db);
            return NULL;
        }
    }

    /* 5. Truncate anything written past log_end_offset (partial last write). */
    {
        uint64_t file_sz = 0;
        if (qdb__file_size(db->fd, &file_sz) != QDB_OK) {
            qdb__free_db(db);
            return NULL;
        }
        if (file_sz > db->log_end_offset) {
            if (qdb__file_truncate(db->fd, db->log_end_offset) != QDB_OK) {
                qdb__free_db(db);
                return NULL;
            }
        }
    }

    /* 6. Scan log: validate all records, truncate uncommitted tail if any. */
    if (validate_log(db) != QDB_OK) {
        qdb__free_db(db);
        return NULL;
    }

    /* 7. Replay log to reconstruct in-memory queue state. */
    if (qdb__replay_log(db) != QDB_OK) {
        qdb__free_db(db);
        return NULL;
    }

    /* 8. Set dirty flag and persist (including updated next_msg_id). */
    db->flags |= QDB_FLAG_DIRTY;
    if (qdb__header_update(db->fd, db) != QDB_OK) {
        qdb__free_db(db);
        return NULL;
    }

    return db;
}

/* -------------------------------------------------------------------------
 * qdb_close
 * ---------------------------------------------------------------------- */

void qdb_close(qdb_t *db)
{
    if (!db) {
        return;
    }
    if (db->fd != QDB__INVALID_FD) {
        db->flags &= ~(uint32_t)(QDB_FLAG_DIRTY | QDB_FLAG_WAL_PRESENT);
        (void)qdb__header_update(db->fd, db);
    }
    qdb__free_db(db);
}

/* -------------------------------------------------------------------------
 * qdb_push
 * ---------------------------------------------------------------------- */

int qdb_push(qdb_t *db, const char *queue, const void *data, size_t len)
{
    size_t             qlen;
    uint8_t            name_len;
    uint32_t           plen;
    uint8_t           *buf;
    uint64_t           msg_id;
    uint64_t           rec_start;
    uint64_t           new_end;
    struct qdb__msg   *m;
    struct qdb__queue *q;
    int                rc;

    /* --- input validation --- */
    if (!db || !queue)                          { return QDB_ERR_INVAL; }
    qlen = strlen(queue);
    if (qlen == 0 || qlen > QDB_QUEUE_NAME_MAX) { return QDB_ERR_INVAL; }
    if (len > 0 && !data)                       { return QDB_ERR_INVAL; }
    if (len > QDB_MSG_MAX_LEN)                  { return QDB_ERR_INVAL; }

    name_len = (uint8_t)qlen;

    /* Safe: len <= QDB_MSG_MAX_LEN (64 MiB), qlen <= 255 */
    plen = (uint32_t)QDB_PUSH_HDR_SIZE + (uint32_t)qlen + (uint32_t)len;

    buf = (uint8_t *)malloc((size_t)plen);
    if (!buf) { return QDB_ERR_NOMEM; }

    msg_id = db->next_msg_id;

    qdb__put_u64le(buf + QDB_PUSH_OFF_MSG_ID,    msg_id);
    buf[QDB_PUSH_OFF_QNAME_LEN] = name_len;
    memcpy(buf + QDB_PUSH_OFF_QNAME, queue, qlen);
    if (len > 0) {
        memcpy(buf + QDB_PUSH_OFF_QNAME + qlen, data, len);
    }

    /* --- durable append --- */
    rec_start = db->log_end_offset;
    new_end   = rec_start;

    rc = qdb__append_record(db->fd, QDB_RT_MSG_PUSH, buf, plen, &new_end);
    free(buf);

    if (rc != QDB_OK) {
        /* Disk write failed.  Nothing in memory has changed. */
        return rc;
    }

    /* Record is on disk.  Persist the updated header fields before
     * touching in-memory state. */
    db->log_end_offset = new_end;
    db->next_msg_id    = msg_id + 1u;

    if (qdb__header_update(db->fd, db) != QDB_OK) {
        /* Record is durable but header update failed; caller must
         * close and reopen to get consistent state. */
        return QDB_ERR_IO;
    }

    /* --- update in-memory state --- */
    m = (struct qdb__msg *)calloc(1, sizeof(*m));
    if (!m) {
        /* OOM after a successful durable write.  The record is on disk and
         * next_msg_id has been advanced.  The caller must close and reopen
         * to restore consistent in-memory state. */
        return QDB_ERR_NOMEM;
    }

    m->id               = msg_id;
    m->data_file_offset = rec_start
                        + (uint64_t)QDB_REC_HDR_SIZE
                        + (uint64_t)QDB_PUSH_HDR_SIZE
                        + (uint64_t)name_len;
    m->data_len         = (uint32_t)len;
    m->queue_name_len   = name_len;
    memcpy(m->queue_name, queue, qlen);
    m->queue_name[qlen] = '\0';
    m->state            = QDB_MSG_STATE_PENDING;

    (void)qdb__msg_insert(db->state, m);

    q = qdb__queue_get_or_create(db->state, queue, name_len);
    if (!q) {
        /* OOM creating queue entry; same caveat as above. */
        return QDB_ERR_NOMEM;
    }

    qdb__queue_pending_append(db->state, q, m);
    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * qdb_msg_free
 * ---------------------------------------------------------------------- */

void qdb_msg_free(qdb_msg_t *msg)
{
    if (!msg) {
        return;
    }
    free(msg->queue);
    free(msg->data);
    msg->id       = 0;
    msg->lease_id = 0;
    msg->queue    = NULL;
    msg->data     = NULL;
    msg->len      = 0;
}

/* -------------------------------------------------------------------------
 * Queue operation stubs
 * ---------------------------------------------------------------------- */

int qdb_pop(qdb_t *db, const char *queue, qdb_msg_t *out_msg)
{
    size_t              qlen;
    uint8_t             name_len;
    struct qdb__queue  *q;
    struct qdb__msg    *m;
    uint64_t            lease_id;
    uint64_t            expiry_us;
    uint8_t             lease_buf[QDB_PAYLOAD_LEASE_SIZE];
    uint64_t            new_end;
    struct qdb__lease  *l;
    char               *q_copy;
    void               *d_copy;
    int                 rc;

    /* --- input validation --- */
    if (!db || !queue || !out_msg) { return QDB_ERR_INVAL; }
    qlen = strlen(queue);
    if (qlen == 0 || qlen > QDB_QUEUE_NAME_MAX) { return QDB_ERR_INVAL; }
    name_len = (uint8_t)qlen;

    /* --- find queue and head message --- */
    q = qdb__queue_get(db->state, queue, name_len);
    if (!q || q->pending_count == 0 || q->pending_head == 0) {
        return QDB_ERR_EMPTY;
    }
    m = qdb__msg_get(db->state, q->pending_head);
    if (!m) { return QDB_ERR_CORRUPT; }

    /* --- pre-allocate return buffers and read data before touching disk --- */
    q_copy = (char *)malloc(qlen + 1u);
    if (!q_copy) { return QDB_ERR_NOMEM; }
    memcpy(q_copy, queue, qlen + 1u);

    d_copy = NULL;
    if (m->data_len > 0) {
        d_copy = malloc((size_t)m->data_len);
        if (!d_copy) {
            free(q_copy);
            return QDB_ERR_NOMEM;
        }
        rc = qdb__read_full(db->fd, d_copy, (size_t)m->data_len,
                            m->data_file_offset);
        if (rc != QDB_OK) {
            free(d_copy);
            free(q_copy);
            return rc;
        }
    }

    /* --- assign lease and build payload --- */
    lease_id  = db->next_lease_id;
    expiry_us = qdb__time_us() + QDB_DEFAULT_LEASE_US;

    qdb__put_u64le(lease_buf + QDB_LEASE_OFF_MSG_ID,   m->id);
    qdb__put_u64le(lease_buf + QDB_LEASE_OFF_EXPIRY,   expiry_us);
    qdb__put_u64le(lease_buf + QDB_LEASE_OFF_LEASE_ID, lease_id);

    /* --- durable append --- */
    new_end = db->log_end_offset;
    rc = qdb__append_record(db->fd, QDB_RT_MSG_LEASE,
                            lease_buf, QDB_PAYLOAD_LEASE_SIZE, &new_end);
    if (rc != QDB_OK) {
        free(d_copy);
        free(q_copy);
        return rc;
    }

    /* --- persist header (log_end_offset) --- */
    db->log_end_offset = new_end;
    db->next_lease_id  = lease_id + 1u;
    if (qdb__header_update(db->fd, db) != QDB_OK) {
        /* Record is durable but header update failed. */
        free(d_copy);
        free(q_copy);
        return QDB_ERR_IO;
    }

    /* --- update in-memory state --- */
    qdb__queue_pending_remove(db->state, q, m);
    m->state           = QDB_MSG_STATE_LEASED;
    m->lease_id        = lease_id;
    m->lease_expiry_us = expiry_us;
    q->leased_count++;

    l = (struct qdb__lease *)calloc(1, sizeof(*l));
    if (l) {
        l->lease_id  = lease_id;
        l->msg_id    = m->id;
        l->expiry_us = expiry_us;
        (void)qdb__lease_insert(db->state, l);
    }

    /* --- populate caller-owned output --- */
    out_msg->id       = m->id;
    out_msg->lease_id = lease_id;
    out_msg->queue    = q_copy;
    out_msg->data     = d_copy;
    out_msg->len      = (size_t)m->data_len;

    return QDB_OK;
}

int qdb_ack(qdb_t *db, uint64_t msg_id)
{
    (void)db; (void)msg_id;
    return QDB_ERR_NOENT;
}

/* -------------------------------------------------------------------------
 * Utilities
 * ---------------------------------------------------------------------- */

const char *qdb_errmsg(int err)
{
    switch (err) {
    case QDB_OK:          return "success";
    case QDB_ERR_IO:      return "I/O error";
    case QDB_ERR_CORRUPT: return "database corrupt or unrecognised format";
    case QDB_ERR_INVAL:   return "invalid argument";
    case QDB_ERR_EMPTY:   return "queue is empty";
    case QDB_ERR_NOENT:   return "message ID not found";
    case QDB_ERR_NOMEM:   return "out of memory";
    case QDB_ERR_LOCKED:  return "database locked by another process";
    default:              return "unknown error";
    }
}

const char *qdb_version(void)
{
    return "0.1.0";
}
