/*
 * test_replay.c — log replay and in-memory state reconstruction tests
 *
 * Tests write raw records directly into the database file (bypassing the
 * stubbed push/pop/ack APIs) then reopen the database and verify that the
 * reconstructed in-memory state is correct.
 *
 * SPDX-License-Identifier: MIT
 */

#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#  if !defined(_FILE_OFFSET_BITS)
#    define _FILE_OFFSET_BITS 64
#  endif
#endif

#include "../src/qdb_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_platform.h"

/* -------------------------------------------------------------------------
 * Minimal test harness
 * ---------------------------------------------------------------------- */

static int g_tests_run    = 0;
static int g_tests_failed = 0;
static int g_test_ok      = 1;

#define ASSERT(expr)                                                        \
    do {                                                                    \
        g_tests_run++;                                                      \
        if (!(expr)) {                                                      \
            fprintf(stderr, "  FAIL  %s:%d  %s\n",                        \
                    __FILE__, __LINE__, #expr);                             \
            g_tests_failed++;                                               \
            g_test_ok = 0;                                                  \
        }                                                                   \
    } while (0)

#define ASSERT_EQ(a, b)    ASSERT((a) == (b))
#define ASSERT_NULL(p)     ASSERT((p) == NULL)
#define ASSERT_NOTNULL(p)  ASSERT((p) != NULL)
#define ASSERT_STREQ(a, b) ASSERT(strcmp((a), (b)) == 0)

static void test_begin(const char *name)
{
    g_test_ok = 1;
    printf("  %-60s", name);
    fflush(stdout);
}

static void test_end(void)
{
    printf("%s\n", g_test_ok ? "ok" : "FAILED");
}

/* -------------------------------------------------------------------------
 * Cleanup helper
 * ---------------------------------------------------------------------- */

static void cleanup(const char *path)
{
    qdb_test_cleanup_files(path);
}

/* -------------------------------------------------------------------------
 * Record-writing helpers
 *
 * These bypass the public API and write raw records directly into db->fd,
 * updating db->log_end_offset and persisting the header after each write.
 * Used to construct specific test scenarios that exercise the replay logic.
 * ---------------------------------------------------------------------- */

static int write_push(qdb_t *db, uint64_t msg_id,
                      const char *qname,
                      const void *data, uint32_t data_len)
{
    size_t   nl   = strlen(qname);
    uint32_t plen = (uint32_t)(QDB_PUSH_HDR_SIZE + nl + (size_t)data_len);
    uint8_t *buf  = (uint8_t *)malloc((size_t)plen);
    int      rc;

    if (!buf) { return QDB_ERR_NOMEM; }
    qdb__put_u64le(buf + QDB_PUSH_OFF_MSG_ID,    msg_id);
    buf[QDB_PUSH_OFF_QNAME_LEN] = (uint8_t)nl;
    memcpy(buf + QDB_PUSH_OFF_QNAME, qname, nl);
    if (data_len > 0) {
        memcpy(buf + QDB_PUSH_OFF_QNAME + nl, data, (size_t)data_len);
    }

    rc = qdb__append_record(db->fd, QDB_RT_MSG_PUSH, buf, plen,
                            &db->log_end_offset);
    free(buf);
    if (rc == QDB_OK) {
        /* Persist next_msg_id so reopened DB has the right value. */
        if (msg_id >= db->next_msg_id) { db->next_msg_id = msg_id + 1u; }
        rc = qdb__header_update(db->fd, db);
    }
    return rc;
}

static int write_lease(qdb_t *db, uint64_t msg_id,
                       uint64_t lease_id, uint64_t expiry_us)
{
    uint8_t buf[QDB_PAYLOAD_LEASE_SIZE];
    int     rc;

    qdb__put_u64le(buf + QDB_LEASE_OFF_MSG_ID,   msg_id);
    qdb__put_u64le(buf + QDB_LEASE_OFF_EXPIRY,   expiry_us);
    qdb__put_u64le(buf + QDB_LEASE_OFF_LEASE_ID, lease_id);

    rc = qdb__append_record(db->fd, QDB_RT_MSG_LEASE, buf,
                            QDB_PAYLOAD_LEASE_SIZE, &db->log_end_offset);
    if (rc == QDB_OK) { rc = qdb__header_update(db->fd, db); }
    return rc;
}

static int write_ack_type(qdb_t *db, uint8_t rtype,
                          uint64_t msg_id, uint64_t lease_id)
{
    uint8_t buf[QDB_PAYLOAD_ACK_SIZE];
    int     rc;

    qdb__put_u64le(buf + QDB_ACK_OFF_MSG_ID,   msg_id);
    qdb__put_u64le(buf + QDB_ACK_OFF_LEASE_ID, lease_id);

    rc = qdb__append_record(db->fd, rtype, buf,
                            QDB_PAYLOAD_ACK_SIZE, &db->log_end_offset);
    if (rc == QDB_OK) { rc = qdb__header_update(db->fd, db); }
    return rc;
}

/* -------------------------------------------------------------------------
 * Count helpers (used with qdb__state_iter_*)
 * ---------------------------------------------------------------------- */

struct count_ctx { int total; int pending; int leased; int acked; };

static void count_msgs(const struct qdb__msg *m, void *ctx)
{
    struct count_ctx *c = (struct count_ctx *)ctx;
    c->total++;
    if (m->state == QDB_MSG_STATE_PENDING) { c->pending++; }
    if (m->state == QDB_MSG_STATE_LEASED)  { c->leased++;  }
    if (m->state == QDB_MSG_STATE_ACKED)   { c->acked++;   }
}

struct queue_find_ctx {
    const char       *name;
    struct qdb__queue found;
    int               hit;
};

static void find_queue(const struct qdb__queue *q, void *ctx)
{
    struct queue_find_ctx *fc = (struct queue_find_ctx *)ctx;
    if (strcmp(q->name, fc->name) == 0) {
        fc->found = *q;
        fc->hit   = 1;
    }
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

static void test_empty_state(void)
{
    const char *path = "rp_empty.qdb";
    test_begin("empty db: state initialised with no messages");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_NOTNULL(db->state);
            ASSERT_EQ(db->state->msg_count,   0u);
            ASSERT_EQ(db->state->queue_count, 0u);
            ASSERT_EQ(db->state->lease_count, 0u);
            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_push_replay(void)
{
    const char *path = "rp_push.qdb";
    test_begin("push replay: 3 messages in PENDING");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db, 1, "jobs", "hello", 5), QDB_OK);
            ASSERT_EQ(write_push(db, 2, "jobs", "world", 5), QDB_OK);
            ASSERT_EQ(write_push(db, 3, "jobs", "three", 5), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            struct count_ctx c = {0,0,0,0};
            qdb__state_iter_msgs(db->state, count_msgs, &c);
            ASSERT_EQ(c.total,   3);
            ASSERT_EQ(c.pending, 3);
            ASSERT_EQ(c.leased,  0);
            ASSERT_EQ(c.acked,   0);

            /* Queue exists with correct counts. */
            struct queue_find_ctx fc;
            memset(&fc, 0, sizeof(fc));
            fc.name = "jobs";
            qdb__state_iter_queues(db->state, find_queue, &fc);
            ASSERT_EQ(fc.hit, 1);
            ASSERT_EQ(fc.found.pending_count, 3u);

            /* FIFO order: head is msg 1. */
            ASSERT_EQ(fc.found.pending_head, 1u);
            ASSERT_EQ(fc.found.pending_tail, 3u);

            /* next_msg_id updated from records. */
            ASSERT_EQ(db->next_msg_id, 4u);

            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_lease_replay(void)
{
    const char *path = "rp_lease.qdb";
    test_begin("lease replay: message transitions to LEASED");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  1, "q", "data", 4), QDB_OK);
            ASSERT_EQ(write_lease(db, 1, 100, 9999999999ull), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            struct count_ctx c = {0,0,0,0};
            qdb__state_iter_msgs(db->state, count_msgs, &c);
            ASSERT_EQ(c.total,  1);
            ASSERT_EQ(c.leased, 1);

            const struct qdb__msg *m = qdb__msg_get(db->state, 1);
            ASSERT_NOTNULL(m);
            if (m) {
                ASSERT_EQ(m->state,    (qdb_msg_state_t)QDB_MSG_STATE_LEASED);
                ASSERT_EQ(m->lease_id, 100u);
            }

            /* Lease table has the entry. */
            ASSERT_EQ(db->state->lease_count, 1u);
            const struct qdb__lease *l = qdb__lease_get(db->state, 100);
            ASSERT_NOTNULL(l);
            if (l) {
                ASSERT_EQ(l->msg_id, 1u);
            }

            /* Queue has no PENDING messages. */
            struct queue_find_ctx fc;
            memset(&fc, 0, sizeof(fc));
            fc.name = "q";
            qdb__state_iter_queues(db->state, find_queue, &fc);
            ASSERT_EQ(fc.hit,                     1);
            ASSERT_EQ(fc.found.pending_count,     0u);
            ASSERT_EQ(fc.found.leased_count,      1u);

            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_ack_replay(void)
{
    const char *path = "rp_ack.qdb";
    test_begin("ack replay: push → lease → ack yields ACKED state");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  1, "q", "x", 1), QDB_OK);
            ASSERT_EQ(write_lease(db, 1, 42, 1000000ull), QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_ACK, 1, 42), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            struct count_ctx c = {0,0,0,0};
            qdb__state_iter_msgs(db->state, count_msgs, &c);
            ASSERT_EQ(c.total, 1);
            ASSERT_EQ(c.acked, 1);

            /* Lease entry must be gone. */
            ASSERT_EQ(db->state->lease_count, 0u);

            struct queue_find_ctx fc;
            memset(&fc, 0, sizeof(fc));
            fc.name = "q";
            qdb__state_iter_queues(db->state, find_queue, &fc);
            ASSERT_EQ(fc.found.acked_count,   1u);
            ASSERT_EQ(fc.found.pending_count, 0u);
            ASSERT_EQ(fc.found.leased_count,  0u);

            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_nack_replay(void)
{
    const char *path = "rp_nack.qdb";
    test_begin("nack replay: push → lease → nack returns to PENDING");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  1, "q", "x", 1), QDB_OK);
            ASSERT_EQ(write_lease(db, 1, 77, 1000ull), QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_NACK, 1, 77), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            const struct qdb__msg *m = qdb__msg_get(db->state, 1);
            ASSERT_NOTNULL(m);
            if (m) {
                ASSERT_EQ(m->state,       (qdb_msg_state_t)QDB_MSG_STATE_PENDING);
                ASSERT_EQ(m->retry_count, 1u);
                ASSERT_EQ(m->lease_id,    0u);
            }
            ASSERT_EQ(db->state->lease_count, 0u);

            struct queue_find_ctx fc;
            memset(&fc, 0, sizeof(fc));
            fc.name = "q";
            qdb__state_iter_queues(db->state, find_queue, &fc);
            ASSERT_EQ(fc.found.pending_count, 1u);
            ASSERT_EQ(fc.found.leased_count,  0u);

            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_expire_replay(void)
{
    const char *path = "rp_expire.qdb";
    test_begin("expire replay: lease expiry returns message to PENDING");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  1, "q", "x", 1), QDB_OK);
            ASSERT_EQ(write_lease(db, 1, 55, 1000ull), QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_EXPIRE, 1, 55), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            const struct qdb__msg *m = qdb__msg_get(db->state, 1);
            ASSERT_NOTNULL(m);
            if (m) {
                ASSERT_EQ(m->state,       (qdb_msg_state_t)QDB_MSG_STATE_PENDING);
                ASSERT_EQ(m->retry_count, 1u);
            }
            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_multiple_queues(void)
{
    const char *path = "rp_multi.qdb";
    test_begin("multiple queues: each queue has independent FIFO");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db, 1, "alpha", "a1", 2), QDB_OK);
            ASSERT_EQ(write_push(db, 2, "beta",  "b1", 2), QDB_OK);
            ASSERT_EQ(write_push(db, 3, "alpha", "a2", 2), QDB_OK);
            ASSERT_EQ(write_push(db, 4, "gamma", "g1", 2), QDB_OK);
            ASSERT_EQ(write_push(db, 5, "beta",  "b2", 2), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            struct count_ctx c = {0,0,0,0};
            qdb__state_iter_msgs(db->state, count_msgs, &c);
            ASSERT_EQ(c.total,   5);
            ASSERT_EQ(c.pending, 5);
            ASSERT_EQ(db->state->queue_count, 3u);

            /* alpha: head=1, tail=3 */
            struct queue_find_ctx fc;
            memset(&fc, 0, sizeof(fc));
            fc.name = "alpha";
            qdb__state_iter_queues(db->state, find_queue, &fc);
            ASSERT_EQ(fc.hit,                  1);
            ASSERT_EQ(fc.found.pending_count,  2u);
            ASSERT_EQ(fc.found.pending_head,   1u);
            ASSERT_EQ(fc.found.pending_tail,   3u);

            /* beta: head=2, tail=5 */
            memset(&fc, 0, sizeof(fc));
            fc.name = "beta";
            qdb__state_iter_queues(db->state, find_queue, &fc);
            ASSERT_EQ(fc.found.pending_count,  2u);
            ASSERT_EQ(fc.found.pending_head,   2u);
            ASSERT_EQ(fc.found.pending_tail,   5u);

            /* gamma: single message */
            memset(&fc, 0, sizeof(fc));
            fc.name = "gamma";
            qdb__state_iter_queues(db->state, find_queue, &fc);
            ASSERT_EQ(fc.found.pending_count,  1u);
            ASSERT_EQ(fc.found.pending_head,   4u);
            ASSERT_EQ(fc.found.pending_tail,   4u);

            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_restart_recovery(void)
{
    const char *path = "rp_restart.qdb";
    test_begin("restart recovery: state survives close/reopen cycle");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  1, "q", "msg1", 4), QDB_OK);
            ASSERT_EQ(write_push(db,  2, "q", "msg2", 4), QDB_OK);
            ASSERT_EQ(write_lease(db, 1, 10, 99999ull), QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_ACK, 1, 10), QDB_OK);
            qdb_close(db);
        }
    }
    /* Open again and verify state was restored. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            struct count_ctx c = {0,0,0,0};
            qdb__state_iter_msgs(db->state, count_msgs, &c);
            ASSERT_EQ(c.total,   2);
            ASSERT_EQ(c.acked,   1);
            ASSERT_EQ(c.pending, 1);

            const struct qdb__msg *m2 = qdb__msg_get(db->state, 2);
            ASSERT_NOTNULL(m2);
            if (m2) {
                ASSERT_EQ(m2->state, (qdb_msg_state_t)QDB_MSG_STATE_PENDING);
            }
            qdb_close(db);
        }
    }
    /* Third open: state must still be consistent. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(db->state->msg_count,  2u);
            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_fifo_order_preserved(void)
{
    const char *path = "rp_fifo.qdb";
    test_begin("FIFO: pending_head points to oldest message");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db, 10, "q", "a", 1), QDB_OK);
            ASSERT_EQ(write_push(db, 20, "q", "b", 1), QDB_OK);
            ASSERT_EQ(write_push(db, 30, "q", "c", 1), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            struct queue_find_ctx fc;
            memset(&fc, 0, sizeof(fc));
            fc.name = "q";
            qdb__state_iter_queues(db->state, find_queue, &fc);
            ASSERT_EQ(fc.found.pending_head, 10u);
            ASSERT_EQ(fc.found.pending_tail, 30u);

            /* Walk the list and verify order: 10 → 20 → 30 */
            const struct qdb__msg *m = qdb__msg_get(db->state, 10);
            ASSERT_NOTNULL(m);
            if (m) { ASSERT_EQ(m->next_pending, 20u); }

            m = qdb__msg_get(db->state, 20);
            ASSERT_NOTNULL(m);
            if (m) {
                ASSERT_EQ(m->prev_pending, 10u);
                ASSERT_EQ(m->next_pending, 30u);
            }

            m = qdb__msg_get(db->state, 30);
            ASSERT_NOTNULL(m);
            if (m) {
                ASSERT_EQ(m->prev_pending,  20u);
                ASSERT_EQ(m->next_pending,  0u);
            }

            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_nack_requeue_order(void)
{
    const char *path = "rp_nack_order.qdb";
    test_begin("nack requeue: nacked message returns to tail");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            /* Push two, lease first, nack it back. */
            ASSERT_EQ(write_push(db,  1, "q", "a", 1), QDB_OK);
            ASSERT_EQ(write_push(db,  2, "q", "b", 1), QDB_OK);
            ASSERT_EQ(write_lease(db, 1, 5, 9999ull), QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_NACK, 1, 5), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            struct queue_find_ctx fc;
            memset(&fc, 0, sizeof(fc));
            fc.name = "q";
            qdb__state_iter_queues(db->state, find_queue, &fc);

            /* After nack, msg 1 is at tail; msg 2 is still at head. */
            ASSERT_EQ(fc.found.pending_head, 2u);
            ASSERT_EQ(fc.found.pending_tail, 1u);
            ASSERT_EQ(fc.found.pending_count, 2u);

            const struct qdb__msg *m1 = qdb__msg_get(db->state, 1);
            ASSERT_NOTNULL(m1);
            if (m1) { ASSERT_EQ(m1->retry_count, 1u); }

            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_duplicate_msg_id(void)
{
    const char *path = "rp_dup.qdb";
    test_begin("duplicate msg_id: qdb_open returns NULL");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db, 1, "q", "a", 1), QDB_OK);
            ASSERT_EQ(write_push(db, 1, "q", "b", 1), QDB_OK); /* duplicate */
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NULL(db);
        if (db) { qdb_close(db); }
    }
    test_end();
    cleanup(path);
}

static void test_ack_unknown_message(void)
{
    const char *path = "rp_ack_unk.qdb";
    test_begin("ack unknown msg_id: qdb_open returns NULL");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            /* Write an ACK for a message that was never pushed. */
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_ACK, 999, 1), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NULL(db);
        if (db) { qdb_close(db); }
    }
    test_end();
    cleanup(path);
}

static void test_ack_wrong_lease(void)
{
    const char *path = "rp_ack_wl.qdb";
    test_begin("ack with wrong lease_id: qdb_open returns NULL");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  1, "q", "x", 1), QDB_OK);
            ASSERT_EQ(write_lease(db, 1, 10, 9999ull), QDB_OK);
            /* ACK with a different lease_id — should be rejected. */
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_ACK, 1, 99), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NULL(db);
        if (db) { qdb_close(db); }
    }
    test_end();
    cleanup(path);
}

static void test_lease_non_pending_message(void)
{
    const char *path = "rp_lease_np.qdb";
    test_begin("lease of already-acked message: qdb_open returns NULL");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  1, "q", "x", 1),          QDB_OK);
            ASSERT_EQ(write_lease(db, 1, 1, 9999ull),            QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_ACK, 1, 1),  QDB_OK);
            /* Attempt to re-lease an already-acked message. */
            ASSERT_EQ(write_lease(db, 1, 2, 9999ull),            QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NULL(db);
        if (db) { qdb_close(db); }
    }
    test_end();
    cleanup(path);
}

static void test_next_msg_id_persisted(void)
{
    const char *path = "rp_nextid.qdb";
    test_begin("next_msg_id derived from push records after reopen");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  7, "q", "x", 1), QDB_OK);
            ASSERT_EQ(write_push(db, 15, "q", "y", 1), QDB_OK);
            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(db->next_msg_id, 16u);
            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

static void test_mixed_sequence(void)
{
    const char *path = "rp_mixed.qdb";
    test_begin("mixed sequence: push/lease/ack/nack/expire all replayed");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(write_push(db,  1, "q", "a", 1), QDB_OK);
            ASSERT_EQ(write_push(db,  2, "q", "b", 1), QDB_OK);
            ASSERT_EQ(write_push(db,  3, "q", "c", 1), QDB_OK);
            ASSERT_EQ(write_push(db,  4, "q", "d", 1), QDB_OK);

            /* Lease and ACK msg 1 */
            ASSERT_EQ(write_lease(db, 1, 100, 9999ull), QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_ACK, 1, 100), QDB_OK);

            /* Lease and NACK msg 2 */
            ASSERT_EQ(write_lease(db, 2, 101, 9999ull), QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_NACK, 2, 101), QDB_OK);

            /* Lease and EXPIRE msg 3 */
            ASSERT_EQ(write_lease(db, 3, 102, 9999ull), QDB_OK);
            ASSERT_EQ(write_ack_type(db, QDB_RT_MSG_EXPIRE, 3, 102), QDB_OK);

            /* Lease msg 4 but leave it leased. */
            ASSERT_EQ(write_lease(db, 4, 103, 9999ull), QDB_OK);

            qdb_close(db);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            struct count_ctx c = {0,0,0,0};
            qdb__state_iter_msgs(db->state, count_msgs, &c);
            ASSERT_EQ(c.total,   4);
            ASSERT_EQ(c.acked,   1);
            ASSERT_EQ(c.pending, 2); /* 2 and 3 are back in pending */
            ASSERT_EQ(c.leased,  1); /* 4 still leased */

            /* retry counts */
            const struct qdb__msg *m2 = qdb__msg_get(db->state, 2);
            const struct qdb__msg *m3 = qdb__msg_get(db->state, 3);
            if (m2) { ASSERT_EQ(m2->retry_count, 1u); }
            if (m3) { ASSERT_EQ(m3->retry_count, 1u); }

            ASSERT_EQ(db->state->lease_count, 1u); /* only msg 4 */

            qdb_close(db);
        }
    }
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB replay tests\n");
    printf("================\n");

    test_empty_state();
    test_push_replay();
    test_lease_replay();
    test_ack_replay();
    test_nack_replay();
    test_expire_replay();
    test_multiple_queues();
    test_restart_recovery();
    test_fifo_order_preserved();
    test_nack_requeue_order();
    test_duplicate_msg_id();
    test_ack_unknown_message();
    test_ack_wrong_lease();
    test_lease_non_pending_message();
    test_next_msg_id_persisted();
    test_mixed_sequence();

    printf("================\n");
    printf("Results: %d/%d passed",
           g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }
    printf("\n");
    return EXIT_SUCCESS;
}
