/*
 * qdb_replay.c — append-only log replay
 *
 * Scans every committed record in the main log file and reconstructs the
 * in-memory queue state: message table, queue FIFO lists, and active lease
 * table.  Performs consistency checks on every record; any violation returns
 * QDB_ERR_CORRUPT so qdb_open() can refuse to open a damaged database.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb_state.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Per-type record handlers
 * ---------------------------------------------------------------------- */

static int replay_push(qdb_t *db, struct qdb__state *st,
                       uint64_t payload_off, uint32_t plen)
{
    /* Fixed prefix: msg_id(8) + name_len(1) = 9 bytes.
     * Then queue name (name_len bytes).  Message data follows. */
    uint8_t         prefix[QDB_PUSH_HDR_SIZE + QDB_QUEUE_NAME_MAX];
    uint64_t        msg_id;
    uint8_t         name_len;
    uint32_t        data_len;
    uint64_t        data_off;
    struct qdb__msg *m;
    struct qdb__queue *q;

    if (plen < QDB_PUSH_MIN_PLEN) {
        return QDB_ERR_CORRUPT;
    }

    /* Read fixed prefix first. */
    if (qdb__read_full(db->fd, prefix, QDB_PUSH_HDR_SIZE, payload_off) != QDB_OK) {
        return QDB_ERR_IO;
    }

    msg_id   = qdb__get_u64le(prefix + QDB_PUSH_OFF_MSG_ID);
    name_len = prefix[QDB_PUSH_OFF_QNAME_LEN];

    /* Upper-bound omitted: name_len is uint8_t so > QDB_QUEUE_NAME_MAX (255)
     * is impossible and would trigger -Wtype-limits on GCC. */
    if (name_len == 0) {
        return QDB_ERR_CORRUPT;
    }
    if (plen < (uint32_t)(QDB_PUSH_HDR_SIZE + (uint32_t)name_len)) {
        return QDB_ERR_CORRUPT;
    }
    if (msg_id == 0) {
        return QDB_ERR_CORRUPT; /* ID 0 is reserved */
    }

    /* Read queue name into remainder of prefix buffer. */
    if (qdb__read_full(db->fd, prefix + QDB_PUSH_HDR_SIZE,
                       (size_t)name_len,
                       payload_off + QDB_PUSH_HDR_SIZE) != QDB_OK) {
        return QDB_ERR_IO;
    }

    /* Duplicate msg_id is a consistency error. */
    if (qdb__msg_get(st, msg_id) != NULL) {
        return QDB_ERR_CORRUPT;
    }

    data_len = plen - (uint32_t)QDB_PUSH_HDR_SIZE - (uint32_t)name_len;
    data_off = payload_off + (uint64_t)QDB_PUSH_HDR_SIZE + (uint64_t)name_len;

    m = (struct qdb__msg *)calloc(1, sizeof(*m));
    if (!m) { return QDB_ERR_NOMEM; }

    m->id               = msg_id;
    m->data_file_offset = data_off;
    m->data_len         = data_len;
    m->queue_name_len   = name_len;
    memcpy(m->queue_name, prefix + QDB_PUSH_HDR_SIZE, (size_t)name_len);
    m->queue_name[(size_t)name_len] = '\0';
    m->state            = QDB_MSG_STATE_PENDING;

    if (qdb__msg_insert(st, m) != QDB_OK) {
        free(m);
        return QDB_ERR_NOMEM;
    }

    q = qdb__queue_get_or_create(st, m->queue_name, name_len);
    if (!q) { return QDB_ERR_NOMEM; }

    qdb__queue_pending_append(st, q, m);

    /* Keep db->next_msg_id consistent with the highest ID seen. */
    if (msg_id >= db->next_msg_id) {
        db->next_msg_id = msg_id + 1u;
    }

    return QDB_OK;
}

static int replay_lease(qdb_t *db, struct qdb__state *st,
                        uint64_t payload_off, uint32_t plen)
{
    uint8_t            buf[QDB_PAYLOAD_LEASE_SIZE];
    uint64_t           msg_id, expiry_us, lease_id;
    struct qdb__msg   *m;
    struct qdb__queue *q;
    struct qdb__lease *l;

    if (plen != QDB_PAYLOAD_LEASE_SIZE) {
        return QDB_ERR_CORRUPT;
    }
    if (qdb__read_full(db->fd, buf, QDB_PAYLOAD_LEASE_SIZE, payload_off) != QDB_OK) {
        return QDB_ERR_IO;
    }

    msg_id   = qdb__get_u64le(buf + QDB_LEASE_OFF_MSG_ID);
    expiry_us = qdb__get_u64le(buf + QDB_LEASE_OFF_EXPIRY);
    lease_id = qdb__get_u64le(buf + QDB_LEASE_OFF_LEASE_ID);

    if (lease_id == 0) { return QDB_ERR_CORRUPT; }

    m = qdb__msg_get(st, msg_id);
    if (!m) { return QDB_ERR_CORRUPT; } /* leasing an unknown message */

    /* A message can only be leased from PENDING state. */
    if (m->state != QDB_MSG_STATE_PENDING) {
        return QDB_ERR_CORRUPT;
    }

    /* lease_id must be unique. */
    if (qdb__lease_get(st, lease_id) != NULL) {
        return QDB_ERR_CORRUPT;
    }

    /* Remove from queue's PENDING list. */
    q = qdb__queue_get(st, m->queue_name, m->queue_name_len);
    if (!q) { return QDB_ERR_CORRUPT; }
    qdb__queue_pending_remove(st, q, m);

    /* Transition to LEASED. */
    m->state            = QDB_MSG_STATE_LEASED;
    m->lease_id         = lease_id;
    m->lease_expiry_us  = expiry_us;
    q->leased_count++;

    /* Record lease in lease table. */
    l = (struct qdb__lease *)calloc(1, sizeof(*l));
    if (!l) { return QDB_ERR_NOMEM; }
    l->lease_id  = lease_id;
    l->msg_id    = msg_id;
    l->expiry_us = expiry_us;
    (void)qdb__lease_insert(st, l);

    if (lease_id >= db->next_lease_id) {
        db->next_lease_id = lease_id + 1u;
    }

    return QDB_OK;
}

static int replay_ack(qdb_t *db, struct qdb__state *st,
                      uint64_t payload_off, uint32_t plen)
{
    uint8_t           buf[QDB_PAYLOAD_ACK_SIZE];
    uint64_t          msg_id, lease_id;
    struct qdb__msg  *m;
    struct qdb__queue *q;

    (void)db;

    if (plen != QDB_PAYLOAD_ACK_SIZE) { return QDB_ERR_CORRUPT; }
    if (qdb__read_full(db->fd, buf, QDB_PAYLOAD_ACK_SIZE, payload_off) != QDB_OK) {
        return QDB_ERR_IO;
    }

    msg_id   = qdb__get_u64le(buf + QDB_ACK_OFF_MSG_ID);
    lease_id = qdb__get_u64le(buf + QDB_ACK_OFF_LEASE_ID);

    m = qdb__msg_get(st, msg_id);
    if (!m) { return QDB_ERR_CORRUPT; }
    if (m->state != QDB_MSG_STATE_LEASED) { return QDB_ERR_CORRUPT; }
    if (m->lease_id != lease_id) { return QDB_ERR_CORRUPT; }

    q = qdb__queue_get(st, m->queue_name, m->queue_name_len);
    if (!q) { return QDB_ERR_CORRUPT; }

    qdb__lease_remove(st, lease_id);

    m->state           = QDB_MSG_STATE_ACKED;
    m->lease_id        = 0;
    m->lease_expiry_us = 0;
    q->leased_count--;
    q->acked_count++;

    return QDB_OK;
}

static int replay_nack(qdb_t *db, struct qdb__state *st,
                       uint64_t payload_off, uint32_t plen)
{
    uint8_t            buf[QDB_PAYLOAD_NACK_SIZE];
    uint64_t           msg_id, lease_id;
    struct qdb__msg   *m;
    struct qdb__queue *q;

    (void)db;

    if (plen != QDB_PAYLOAD_NACK_SIZE) { return QDB_ERR_CORRUPT; }
    if (qdb__read_full(db->fd, buf, QDB_PAYLOAD_NACK_SIZE, payload_off) != QDB_OK) {
        return QDB_ERR_IO;
    }

    msg_id   = qdb__get_u64le(buf + QDB_ACK_OFF_MSG_ID);
    lease_id = qdb__get_u64le(buf + QDB_ACK_OFF_LEASE_ID);

    m = qdb__msg_get(st, msg_id);
    if (!m) { return QDB_ERR_CORRUPT; }
    if (m->state != QDB_MSG_STATE_LEASED) { return QDB_ERR_CORRUPT; }
    if (m->lease_id != lease_id) { return QDB_ERR_CORRUPT; }

    q = qdb__queue_get(st, m->queue_name, m->queue_name_len);
    if (!q) { return QDB_ERR_CORRUPT; }

    qdb__lease_remove(st, lease_id);

    m->state           = QDB_MSG_STATE_PENDING;
    m->lease_id        = 0;
    m->lease_expiry_us = 0;
    m->retry_count++;
    q->leased_count--;

    /* Return to tail of the pending list. */
    qdb__queue_pending_append(st, q, m);

    return QDB_OK;
}

static int replay_expire(qdb_t *db, struct qdb__state *st,
                         uint64_t payload_off, uint32_t plen)
{
    /* EXPIRE is semantically identical to NACK for recovery purposes:
     * the lease expires and the message re-enters the pending queue. */
    return replay_nack(db, st, payload_off, plen);
}

static int replay_checkpoint(qdb_t *db, struct qdb__state *st,
                              uint64_t payload_off, uint32_t plen)
{
    uint8_t  buf[QDB_PAYLOAD_CHECKPOINT_SIZE];
    uint64_t next_msg_id, next_lease_id;

    (void)st;

    if (plen != QDB_PAYLOAD_CHECKPOINT_SIZE) { return QDB_ERR_CORRUPT; }
    if (qdb__read_full(db->fd, buf, QDB_PAYLOAD_CHECKPOINT_SIZE,
                       payload_off) != QDB_OK) {
        return QDB_ERR_IO;
    }

    next_msg_id   = qdb__get_u64le(buf + QDB_CKPT_OFF_NEXT_MSG_ID);
    next_lease_id = qdb__get_u64le(buf + QDB_CKPT_OFF_NEXT_LEASE_ID);

    /* Checkpoint records the counters at the time of compaction.  Only
     * advance them — never regress. */
    if (next_msg_id > db->next_msg_id)     { db->next_msg_id   = next_msg_id; }
    if (next_lease_id > db->next_lease_id) { db->next_lease_id = next_lease_id; }

    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */

int qdb__replay_log(qdb_t *db)
{
    struct qdb__state *st;
    uint64_t           offset;
    int                rc;

    st = qdb__state_alloc();
    if (!st) { return QDB_ERR_NOMEM; }

    offset = db->log_start_offset;

    while (offset < db->log_end_offset) {
        uint8_t  type;
        uint32_t plen;
        uint64_t rec_start = offset;

        rc = qdb__scan_record(db->fd, &offset, db->log_end_offset, &type, &plen);
        if (rc == QDB__SCAN_END || rc == QDB__SCAN_PARTIAL) {
            /* validate_log() already truncated any partial tail; nothing to do. */
            break;
        }
        if (rc != QDB_OK) {
            qdb__state_free(st);
            return rc;
        }

        /* payload starts immediately after the record header */
        uint64_t payload_off = rec_start + (uint64_t)QDB_REC_HDR_SIZE;

        switch (type) {
        case QDB_RT_MSG_PUSH:
            rc = replay_push(db, st, payload_off, plen);
            break;
        case QDB_RT_MSG_LEASE:
            rc = replay_lease(db, st, payload_off, plen);
            break;
        case QDB_RT_MSG_ACK:
            rc = replay_ack(db, st, payload_off, plen);
            break;
        case QDB_RT_MSG_NACK:
            rc = replay_nack(db, st, payload_off, plen);
            break;
        case QDB_RT_MSG_EXPIRE:
            rc = replay_expire(db, st, payload_off, plen);
            break;
        case QDB_RT_CHECKPOINT:
            rc = replay_checkpoint(db, st, payload_off, plen);
            break;
        case QDB_RT_PADDING:
            rc = QDB_OK;
            break;
        default:
            /* qdb__scan_record rejects unknown types; this is unreachable. */
            rc = QDB_ERR_CORRUPT;
            break;
        }

        if (rc != QDB_OK) {
            qdb__state_free(st);
            return rc;
        }
    }

    db->state = st;
    return QDB_OK;
}
