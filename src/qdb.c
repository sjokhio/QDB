/*
 * qdb.c — database lifecycle and public API
 *
 * Implements the full public API: open/close, push/pop/ack/nack,
 * process_expired_leases, compact, stats, and utility functions.
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
 * file.  The WAL write path is not currently wired to push/pop/ack (all
 * writes go directly to the main log); this function handles any WAL file
 * left by a future write path without requiring changes to open/recovery.
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
 * qdb_open / qdb_open_ex
 * ---------------------------------------------------------------------- */

qdb_t *qdb_open_ex(const char *path, const qdb_open_opts_t *opts)
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

    /* Apply lease timeout from opts; 0 or absent → library default. */
    db->lease_timeout_us = (opts && opts->lease_timeout_s != 0u)
                           ? (uint64_t)opts->lease_timeout_s * UINT64_C(1000000)
                           : QDB_DEFAULT_LEASE_US;

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

    /* Delete stale compact staging file left by an interrupted qdb_compact().
     * Must run after lock acquisition so no other writer is active. */
    {
        char *stale = make_sidecar(path, path_len, "-compact");
        if (stale) {
            (void)qdb__file_delete(stale);
            free(stale);
        }
        /* OOM here is non-fatal: stale cleanup is best-effort. */
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

    /* next_lease_id is not stored in the header.  Seed at 1 (the same value
     * a new database starts with) so that a push-only database never issues
     * lease ID 0.  qdb__replay_log() advances this past any LEASE or
     * CHECKPOINT records found in the log. */
    db->next_lease_id = 1u;

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

qdb_t *qdb_open(const char *path)
{
    return qdb_open_ex(path, NULL);
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
    msg->id          = 0;
    msg->lease_id    = 0;
    msg->queue       = NULL;
    msg->data        = NULL;
    msg->len         = 0;
    msg->retry_count = 0;
}

/* -------------------------------------------------------------------------
 * Queue operations
 *
 * do_pop() contains the lease-issuance logic shared by qdb_pop() and
 * qdb_pop_any().  The caller locates the target queue and message; do_pop()
 * allocates return buffers, writes the durable LEASE record, updates
 * in-memory state, and populates out_msg.
 * ---------------------------------------------------------------------- */

static int do_pop(qdb_t *db, struct qdb__queue *q, struct qdb__msg *m,
                  qdb_msg_t *out_msg)
{
    uint64_t           lease_id;
    uint64_t           expiry_us;
    uint8_t            lease_buf[QDB_PAYLOAD_LEASE_SIZE];
    uint64_t           new_end;
    struct qdb__lease *l;
    char              *q_copy;
    void              *d_copy;
    int                rc;

    /*
     * Pre-allocate all heap objects before touching disk.  This guarantees
     * that if any allocation fails we return QDB_ERR_NOMEM with no disk side
     * effects and no in-memory state changes.
     *
     * The lease struct must be allocated here (not after the disk write)
     * because qdb__lease_insert() is a pure pointer-link with no further
     * allocation.  Once the durable LEASE record is on disk we commit the
     * insert unconditionally — a successful pop must never leave a lease
     * absent from lease_buckets.
     */
    q_copy = (char *)malloc((size_t)m->queue_name_len + 1u);
    if (!q_copy) { return QDB_ERR_NOMEM; }
    memcpy(q_copy, m->queue_name, (size_t)m->queue_name_len + 1u);

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

    l = (struct qdb__lease *)calloc(1, sizeof(*l));
    if (!l) {
        free(d_copy);
        free(q_copy);
        return QDB_ERR_NOMEM;
    }

    /* --- assign lease and build payload --- */
    lease_id  = db->next_lease_id;
    expiry_us = qdb__time_us() + db->lease_timeout_us;

    qdb__put_u64le(lease_buf + QDB_LEASE_OFF_MSG_ID,   m->id);
    qdb__put_u64le(lease_buf + QDB_LEASE_OFF_EXPIRY,   expiry_us);
    qdb__put_u64le(lease_buf + QDB_LEASE_OFF_LEASE_ID, lease_id);

    /* --- durable append --- */
    new_end = db->log_end_offset;
    rc = qdb__append_record(db->fd, QDB_RT_MSG_LEASE,
                            lease_buf, QDB_PAYLOAD_LEASE_SIZE, &new_end);
    if (rc != QDB_OK) {
        free(l);
        free(d_copy);
        free(q_copy);
        return rc;
    }

    /* --- persist header --- */
    db->log_end_offset = new_end;
    db->next_lease_id  = lease_id + 1u;
    if (qdb__header_update(db->fd, db) != QDB_OK) {
        /*
         * The LEASE record is durable; header update failure leaves the
         * database in a recoverable state (replay will reconstruct on the
         * next qdb_open).  Free the pre-allocated lease struct and return
         * the error so the caller is aware of the partial write.
         */
        free(l);
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

    /* qdb__lease_insert() links l into lease_buckets — no allocation, cannot fail. */
    l->lease_id  = lease_id;
    l->msg_id    = m->id;
    l->expiry_us = expiry_us;
    (void)qdb__lease_insert(db->state, l);

    /* --- populate caller-owned output --- */
    out_msg->id          = m->id;
    out_msg->lease_id    = lease_id;
    out_msg->queue       = q_copy;
    out_msg->data        = d_copy;
    out_msg->len         = (size_t)m->data_len;
    out_msg->retry_count = m->retry_count;

    return QDB_OK;
}

int qdb_pop(qdb_t *db, const char *queue, qdb_msg_t *out_msg)
{
    size_t             qlen;
    uint8_t            name_len;
    struct qdb__queue *q;
    struct qdb__msg   *m;

    if (!db || !queue || !out_msg) { return QDB_ERR_INVAL; }
    qlen = strlen(queue);
    if (qlen == 0 || qlen > QDB_QUEUE_NAME_MAX) { return QDB_ERR_INVAL; }
    name_len = (uint8_t)qlen;

    q = qdb__queue_get(db->state, queue, name_len);
    if (!q || q->pending_count == 0 || q->pending_head == 0) {
        return QDB_ERR_EMPTY;
    }
    m = qdb__msg_get(db->state, q->pending_head);
    if (!m) { return QDB_ERR_CORRUPT; }

    return do_pop(db, q, m, out_msg);
}

/* -------------------------------------------------------------------------
 * qdb_pop_any
 * ---------------------------------------------------------------------- */

int qdb_pop_any(qdb_t *db, qdb_msg_t *out_msg)
{
    uint32_t           b;
    struct qdb__queue *best_q  = NULL;
    struct qdb__msg   *best_m  = NULL;
    uint64_t           best_id = (uint64_t)-1;  /* UINT64_MAX sentinel */

    if (!db || !out_msg) { return QDB_ERR_INVAL; }

    /*
     * Scan every queue; pick the one whose pending_head has the lowest
     * msg_id.  Because msg_id is a global monotonic counter assigned at
     * push time, the minimum pending_head is the oldest currently-available
     * message in the database.
     *
     * O(Q) where Q is the number of queue entries.  For databases with a
     * very large number of queues a min-heap indexed by pending_head.msg_id
     * would reduce this to O(log Q), but O(Q) is appropriate for v1.1.
     */
    for (b = 0; b < QDB__QUEUE_BUCKETS; b++) {
        struct qdb__queue *q = db->state->queue_buckets[b];
        while (q) {
            if (q->pending_count > 0 && q->pending_head != 0) {
                struct qdb__msg *m = qdb__msg_get(db->state, q->pending_head);
                if (!m) { return QDB_ERR_CORRUPT; }
                if (m->id < best_id) {
                    best_id = m->id;
                    best_q  = q;
                    best_m  = m;
                }
            }
            q = q->next_in_bucket;
        }
    }

    if (!best_q) { return QDB_ERR_EMPTY; }

    return do_pop(db, best_q, best_m, out_msg);
}

int qdb_ack(qdb_t *db, uint64_t msg_id, uint64_t lease_id)
{
    struct qdb__msg   *m;
    struct qdb__queue *q;
    uint8_t            ack_buf[QDB_PAYLOAD_ACK_SIZE];
    uint64_t           new_end;
    int                rc;

    if (!db) { return QDB_ERR_INVAL; }

    m = qdb__msg_get(db->state, msg_id);
    if (!m)                              { return QDB_ERR_NOENT; }
    if (m->state == QDB_MSG_STATE_ACKED) { return QDB_ERR_NOENT; }
    if (m->state != QDB_MSG_STATE_LEASED){ return QDB_ERR_NOENT; }
    if (m->lease_id != lease_id)         { return QDB_ERR_INVAL; }

    qdb__put_u64le(ack_buf + QDB_ACK_OFF_MSG_ID,   msg_id);
    qdb__put_u64le(ack_buf + QDB_ACK_OFF_LEASE_ID, lease_id);

    /* --- durable append --- */
    new_end = db->log_end_offset;
    rc = qdb__append_record(db->fd, QDB_RT_MSG_ACK,
                            ack_buf, QDB_PAYLOAD_ACK_SIZE, &new_end);
    if (rc != QDB_OK) { return rc; }

    /* --- persist header --- */
    db->log_end_offset = new_end;
    if (qdb__header_update(db->fd, db) != QDB_OK) {
        return QDB_ERR_IO;
    }

    /* --- update in-memory state --- */
    q = qdb__queue_get(db->state, m->queue_name, m->queue_name_len);
    if (q) {
        q->leased_count--;
        q->acked_count++;
    }

    qdb__lease_remove(db->state, lease_id);

    m->state           = QDB_MSG_STATE_ACKED;
    m->lease_id        = 0;
    m->lease_expiry_us = 0;

    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * qdb_nack
 * ---------------------------------------------------------------------- */

int qdb_nack(qdb_t *db, uint64_t msg_id, uint64_t lease_id)
{
    struct qdb__msg   *m;
    struct qdb__queue *q;
    uint8_t            nack_buf[QDB_PAYLOAD_NACK_SIZE];
    uint64_t           new_end;
    int                rc;

    if (!db) { return QDB_ERR_INVAL; }

    m = qdb__msg_get(db->state, msg_id);
    if (!m)                               { return QDB_ERR_NOENT; }
    if (m->state == QDB_MSG_STATE_ACKED)  { return QDB_ERR_NOENT; }
    if (m->state != QDB_MSG_STATE_LEASED) { return QDB_ERR_NOENT; }
    if (m->lease_id != lease_id)          { return QDB_ERR_INVAL; }

    qdb__put_u64le(nack_buf + QDB_ACK_OFF_MSG_ID,   msg_id);
    qdb__put_u64le(nack_buf + QDB_ACK_OFF_LEASE_ID, lease_id);

    /* --- durable append --- */
    new_end = db->log_end_offset;
    rc = qdb__append_record(db->fd, QDB_RT_MSG_NACK,
                            nack_buf, QDB_PAYLOAD_NACK_SIZE, &new_end);
    if (rc != QDB_OK) { return rc; }

    /* --- persist header --- */
    db->log_end_offset = new_end;
    if (qdb__header_update(db->fd, db) != QDB_OK) {
        return QDB_ERR_IO;
    }

    /* --- update in-memory state --- */
    q = qdb__queue_get(db->state, m->queue_name, m->queue_name_len);

    qdb__lease_remove(db->state, lease_id);

    m->state           = QDB_MSG_STATE_PENDING;
    m->lease_id        = 0;
    m->lease_expiry_us = 0;
    m->retry_count++;

    if (q) {
        q->leased_count--;
        qdb__queue_pending_append(db->state, q, m);
    }

    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * qdb_process_expired_leases
 * ---------------------------------------------------------------------- */

static int expire_one(qdb_t *db, uint64_t lease_id, uint64_t msg_id)
{
    struct qdb__msg   *m;
    struct qdb__queue *q;
    uint8_t            buf[QDB_PAYLOAD_EXPIRE_SIZE];
    uint64_t           new_end;
    int                rc;

    m = qdb__msg_get(db->state, msg_id);
    if (!m || m->state != QDB_MSG_STATE_LEASED || m->lease_id != lease_id) {
        return 0; /* stale entry — skip without error */
    }

    qdb__put_u64le(buf + QDB_ACK_OFF_MSG_ID,   msg_id);
    qdb__put_u64le(buf + QDB_ACK_OFF_LEASE_ID, lease_id);

    new_end = db->log_end_offset;
    rc = qdb__append_record(db->fd, QDB_RT_MSG_EXPIRE,
                            buf, QDB_PAYLOAD_EXPIRE_SIZE, &new_end);
    if (rc != QDB_OK) { return rc; }

    db->log_end_offset = new_end;
    if (qdb__header_update(db->fd, db) != QDB_OK) { return QDB_ERR_IO; }

    q = qdb__queue_get(db->state, m->queue_name, m->queue_name_len);

    qdb__lease_remove(db->state, lease_id);

    m->state           = QDB_MSG_STATE_PENDING;
    m->lease_id        = 0;
    m->lease_expiry_us = 0;
    m->retry_count++;

    if (q) {
        q->leased_count--;
        qdb__queue_pending_append(db->state, q, m);
    }

    return 0;
}

int qdb_process_expired_leases(qdb_t *db)
{
    uint64_t  now_us;
    int       processed = 0;
    uint32_t  b;

    if (!db) { return QDB_ERR_INVAL; }

    now_us = qdb__time_us();

    for (b = 0; b < QDB__LEASE_BUCKETS; b++) {
        struct qdb__lease *l = db->state->lease_buckets[b];
        while (l != NULL) {
            /* Save next before expire_one may free l via qdb__lease_remove */
            struct qdb__lease *next = l->next_in_bucket;
            if (l->expiry_us < now_us) {
                int rc = expire_one(db, l->lease_id, l->msg_id);
                if (rc != QDB_OK) { return rc; }
                processed++;
            }
            l = next;
        }
    }

    return processed;
}

/* -------------------------------------------------------------------------
 * qdb_compact helpers
 * ---------------------------------------------------------------------- */

static int compact_write_push(qdb__fd_t src_fd, qdb__fd_t dst_fd,
                               const struct qdb__msg *m, uint64_t *offset)
{
    uint32_t  plen;
    uint8_t  *buf;
    int       rc;

    /* Safe: data_len <= QDB_MSG_MAX_LEN (64 MiB), name_len <= 255 */
    plen = (uint32_t)QDB_PUSH_HDR_SIZE
         + (uint32_t)m->queue_name_len
         + (uint32_t)m->data_len;

    buf = (uint8_t *)malloc((size_t)plen);
    if (!buf) { return QDB_ERR_NOMEM; }

    qdb__put_u64le(buf + QDB_PUSH_OFF_MSG_ID,    m->id);
    buf[QDB_PUSH_OFF_QNAME_LEN] = m->queue_name_len;
    memcpy(buf + QDB_PUSH_OFF_QNAME, m->queue_name, (size_t)m->queue_name_len);

    if (m->data_len > 0) {
        rc = qdb__read_full(src_fd,
                            buf + QDB_PUSH_OFF_QNAME + (size_t)m->queue_name_len,
                            (size_t)m->data_len, m->data_file_offset);
        if (rc != QDB_OK) { free(buf); return rc; }
    }

    rc = qdb__append_record(dst_fd, QDB_RT_MSG_PUSH, buf, plen, offset);
    free(buf);
    return rc;
}

static int compact_write_lease(qdb__fd_t dst_fd, const struct qdb__msg *m,
                                uint64_t *offset)
{
    uint8_t buf[QDB_PAYLOAD_LEASE_SIZE];

    qdb__put_u64le(buf + QDB_LEASE_OFF_MSG_ID,   m->id);
    qdb__put_u64le(buf + QDB_LEASE_OFF_EXPIRY,   m->lease_expiry_us);
    qdb__put_u64le(buf + QDB_LEASE_OFF_LEASE_ID, m->lease_id);

    return qdb__append_record(dst_fd, QDB_RT_MSG_LEASE, buf,
                              QDB_PAYLOAD_LEASE_SIZE, offset);
}

static int compact_write_checkpoint(qdb__fd_t dst_fd, const qdb_t *db,
                                     uint64_t *offset)
{
    uint8_t buf[QDB_PAYLOAD_CHECKPOINT_SIZE];

    qdb__put_u64le(buf + QDB_CKPT_OFF_TIME_US,       qdb__time_us());
    qdb__put_u64le(buf + QDB_CKPT_OFF_NEXT_MSG_ID,   db->next_msg_id);
    qdb__put_u64le(buf + QDB_CKPT_OFF_NEXT_LEASE_ID, db->next_lease_id);

    return qdb__append_record(dst_fd, QDB_RT_CHECKPOINT, buf,
                              QDB_PAYLOAD_CHECKPOINT_SIZE, offset);
}

/*
 * Re-open the database from db->path after a successful compact rename.
 * Equivalent to qdb_open_ex() steps 3–8 without re-acquiring the lock
 * (the caller already holds db->lock_fd).
 */
static int qdb__reopen_after_compact(qdb_t *db)
{
    int      is_new;
    int      rc;
    uint64_t file_sz;

    /* Close only if still open.  On Windows, qdb_compact() must close db->fd
     * before MoveFileExA; on POSIX it is still open here. */
    if (db->fd != QDB__INVALID_FD) {
        qdb__file_close(db->fd);
        db->fd = QDB__INVALID_FD;
    }

    rc = qdb__file_open(db->path, 0, &db->fd, &is_new);
    if (rc != QDB_OK) { goto fail; }

    rc = qdb__header_read(db->fd, db);
    if (rc != QDB_OK) { goto fail; }

    /* Seed next_lease_id before replay (same logic as qdb_open_ex). */
    db->next_lease_id = 1u;

    /* Compact file always has DIRTY set; replay_wal finds no WAL → QDB_OK. */
    if ((db->flags & QDB_FLAG_WAL_PRESENT) || (db->flags & QDB_FLAG_DIRTY)) {
        rc = replay_wal(db);
        if (rc != QDB_OK) { goto fail; }
    }

    /* Truncate anything written past log_end_offset. */
    if (qdb__file_size(db->fd, &file_sz) != QDB_OK) { rc = QDB_ERR_IO; goto fail; }
    if (file_sz > db->log_end_offset) {
        if (qdb__file_truncate(db->fd, db->log_end_offset) != QDB_OK) {
            rc = QDB_ERR_IO;
            goto fail;
        }
    }

    /* Validate log: scan records, truncate uncommitted tail if found. */
    rc = validate_log(db);
    if (rc != QDB_OK) { goto fail; }

    /* Rebuild in-memory state from the compact file. */
    qdb__state_free(db->state);
    db->state = NULL;

    rc = qdb__replay_log(db);
    if (rc != QDB_OK) { goto fail; }

    db->flags |= QDB_FLAG_DIRTY;
    rc = qdb__header_update(db->fd, db);
    if (rc != QDB_OK) { goto fail; }
    return QDB_OK;

fail:
    /* The database file has already been replaced by the compact file.
     * Recovery is not possible in this function; leave the handle in a
     * defined invalid state so subsequent API calls return errors rather
     * than crashing on a stale fd or NULL state pointer. */
    if (db->fd != QDB__INVALID_FD) {
        qdb__file_close(db->fd);
        db->fd = QDB__INVALID_FD;
    }
    qdb__state_free(db->state);
    db->state = NULL;
    return rc;
}

/* -------------------------------------------------------------------------
 * qdb_compact
 * ---------------------------------------------------------------------- */

int qdb_compact(qdb_t *db)
{
    char      *compact_path = NULL;
    size_t     path_len;
    qdb__fd_t  compact_fd  = QDB__INVALID_FD;
    int        is_new;
    int        rc;
    struct qdb cmeta;     /* header-field tracking for the staging file */
    uint64_t   compact_end;
    uint32_t   b;

    if (!db) { return QDB_ERR_INVAL; }

    path_len     = strlen(db->path);
    compact_path = make_sidecar(db->path, path_len, "-compact");
    if (!compact_path) { return QDB_ERR_NOMEM; }

    /* Remove any stale staging file from a prior interrupted compact. */
    (void)qdb__file_delete(compact_path);

    /* Create the staging file. */
    rc = qdb__file_open(compact_path, 1, &compact_fd, &is_new);
    if (rc != QDB_OK) { goto cleanup; }

    /* Write the initial header.  log_end will be updated once at the end;
     * no per-record header updates are written to the staging file. */
    memset(&cmeta, 0, sizeof(cmeta));
    cmeta.create_time_us   = db->create_time_us;
    cmeta.next_msg_id      = db->next_msg_id;
    cmeta.next_lease_id    = db->next_lease_id;
    cmeta.log_start_offset = QDB_HDR_SIZE;
    cmeta.log_end_offset   = QDB_HDR_SIZE;
    cmeta.flags            = QDB_FLAG_DIRTY;

    rc = qdb__header_write(compact_fd, &cmeta);
    if (rc != QDB_OK) { goto cleanup; }

    compact_end = QDB_HDR_SIZE;

    /* PENDING messages: iterate each queue's FIFO chain (head → tail). */
    for (b = 0; b < QDB__QUEUE_BUCKETS; b++) {
        const struct qdb__queue *q = db->state->queue_buckets[b];
        while (q) {
            uint64_t m_id = q->pending_head;
            while (m_id != 0) {
                const struct qdb__msg *m = qdb__msg_get(db->state, m_id);
                if (!m) { rc = QDB_ERR_CORRUPT; goto cleanup; }
                rc = compact_write_push(db->fd, compact_fd, m, &compact_end);
                if (rc != QDB_OK) { goto cleanup; }
                m_id = m->next_pending;
            }
            q = q->next_in_bucket;
        }
    }

    /* LEASED messages: one PUSH record then one LEASE record each. */
    for (b = 0; b < QDB__LEASE_BUCKETS; b++) {
        const struct qdb__lease *l = db->state->lease_buckets[b];
        while (l) {
            const struct qdb__msg *m = qdb__msg_get(db->state, l->msg_id);
            if (!m) { rc = QDB_ERR_CORRUPT; goto cleanup; }
            rc = compact_write_push(db->fd, compact_fd, m, &compact_end);
            if (rc != QDB_OK) { goto cleanup; }
            rc = compact_write_lease(compact_fd, m, &compact_end);
            if (rc != QDB_OK) { goto cleanup; }
            l = l->next_in_bucket;
        }
    }

    /* CHECKPOINT — always the final record, pins counter monotonicity. */
    rc = compact_write_checkpoint(compact_fd, db, &compact_end);
    if (rc != QDB_OK) { goto cleanup; }

    /* Single header update for all records at once, then full fsync. */
    cmeta.log_end_offset = compact_end;
    rc = qdb__header_update(compact_fd, &cmeta);
    if (rc != QDB_OK) { goto cleanup; }

    rc = qdb__file_sync(compact_fd);
    if (rc != QDB_OK) { goto cleanup; }

    qdb__file_close(compact_fd);
    compact_fd = QDB__INVALID_FD;

    /* On Windows, MoveFileExA(MOVEFILE_REPLACE_EXISTING) fails when the
     * destination has any open handle, regardless of FILE_SHARE_DELETE.
     * Close db->fd before the rename; qdb__reopen_after_compact() will
     * reopen it afterwards.  Its guard handles the already-closed case. */
#if defined(_WIN32)
    qdb__file_close(db->fd);
    db->fd = QDB__INVALID_FD;
#endif

    /* Atomic rename: the staging file becomes the live database. */
    rc = qdb__file_rename(compact_path, db->path);
    if (rc != QDB_OK) {
        (void)qdb__file_delete(compact_path);
        free(compact_path);
        return rc;
    }

    free(compact_path);

    /* Rebuild in-memory state from the compacted file. */
    return qdb__reopen_after_compact(db);

cleanup:
    if (compact_fd != QDB__INVALID_FD) {
        qdb__file_close(compact_fd);
    }
    (void)qdb__file_delete(compact_path);
    free(compact_path);
    return rc;
}

/* -------------------------------------------------------------------------
 * qdb_stats / qdb_queue_stats
 * ---------------------------------------------------------------------- */

int qdb_stats(qdb_t *db, qdb_stats_t *out)
{
    struct qdb__state *st;
    uint32_t           b;

    if (!db || !out) { return QDB_ERR_INVAL; }

    memset(out, 0, sizeof(*out));
    st = db->state;
    out->queue_count = st->queue_count;

    for (b = 0; b < QDB__QUEUE_BUCKETS; b++) {
        const struct qdb__queue *q = st->queue_buckets[b];
        while (q) {
            out->pending_count += q->pending_count;
            out->leased_count  += q->leased_count;
            out->acked_count   += q->acked_count;
            q = q->next_in_bucket;
        }
    }

    (void)qdb__file_size(db->fd, &out->file_size_bytes);

    return QDB_OK;
}

int qdb_queue_stats(qdb_t *db, const char *queue, qdb_queue_stats_t *out)
{
    const struct qdb__queue *q;
    size_t                   qlen;

    if (!db || !queue || !out) { return QDB_ERR_INVAL; }
    qlen = strlen(queue);
    if (qlen == 0 || qlen > QDB_QUEUE_NAME_MAX) { return QDB_ERR_INVAL; }

    q = qdb__queue_get(db->state, queue, (uint8_t)qlen);
    if (!q) { return QDB_ERR_NOENT; }

    memset(out, 0, sizeof(*out));
    out->pending_count = q->pending_count;
    out->leased_count  = q->leased_count;
    out->acked_count   = q->acked_count;

    return QDB_OK;
}

int qdb_queue_list(qdb_t *db,
                   qdb_queue_name_t *out, size_t cap,
                   size_t *out_count)
{
    struct qdb__state *st;
    size_t             total = 0;
    uint32_t           b;

    if (!db || !out_count)  { return QDB_ERR_INVAL; }
    if (!out && cap > 0)    { return QDB_ERR_INVAL; }

    st = db->state;

    for (b = 0; b < QDB__QUEUE_BUCKETS; b++) {
        const struct qdb__queue *q = st->queue_buckets[b];
        while (q) {
            if (out && total < cap) {
                memcpy(out[total].name, q->name, (size_t)q->name_len + 1u);
            }
            total++;
            q = q->next_in_bucket;
        }
    }

    *out_count = total;
    return QDB_OK;
}

int qdb_compact_recommended(qdb_t *db, int *out_recommended)
{
    qdb_stats_t st = {0};
    int         rc;

    if (!db || !out_recommended) { return QDB_ERR_INVAL; }

    rc = qdb_stats(db, &st);
    if (rc != QDB_OK) { return rc; }

    *out_recommended = (st.acked_count > 0) &&
                       (st.acked_count > st.pending_count + st.leased_count);
    return QDB_OK;
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
    return QDB_VERSION_STRING;
}
