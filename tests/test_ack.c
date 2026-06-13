/*
 * test_ack.c — qdb_ack() tests
 *
 * Exercises input validation, lease validation, in-memory state transitions,
 * durability across close/reopen, and the guarantee that a failed disk write
 * leaves in-memory state completely unchanged.
 *
 * SPDX-License-Identifier: MIT
 */

#if !defined(_WIN32)
#  if !defined(_DARWIN_C_SOURCE)
#    define _DARWIN_C_SOURCE
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
#define ASSERT_NE(a, b)    ASSERT((a) != (b))
#define ASSERT_NULL(p)     ASSERT((p) == NULL)
#define ASSERT_NOTNULL(p)  ASSERT((p) != NULL)

static void test_begin(const char *name)
{
    g_test_ok = 1;
    printf("  %-65s", name);
    fflush(stdout);
}
static void test_end(void) { printf("%s\n", g_test_ok ? "ok" : "FAILED"); }

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void cleanup(const char *path)
{
    qdb_test_cleanup_files(path);
}

static qdb_t *open_fresh(const char *path)
{
    cleanup(path);
    return qdb_open(path);
}

/* Push, pop, and return the msg.  Caller calls qdb_msg_free when done. */
static int push_and_pop(qdb_t *db, const char *queue,
                        const void *data, size_t len,
                        qdb_msg_t *out)
{
    int rc = qdb_push(db, queue, data, len);
    if (rc != QDB_OK) { return rc; }
    return qdb_pop(db, queue, out);
}

/* -------------------------------------------------------------------------
 * Test: invalid arguments
 * ---------------------------------------------------------------------- */

static void test_ack_invalid_args(void)
{
    test_begin("invalid args: NULL db returns QDB_ERR_INVAL");

    /* NULL db */
    ASSERT_EQ(qdb_ack(NULL, 1, 1), QDB_ERR_INVAL);

    test_end();
}

/* -------------------------------------------------------------------------
 * Test: ack nonexistent message
 * ---------------------------------------------------------------------- */

static void test_ack_nonexistent(void)
{
    test_begin("ack unknown msg_id returns QDB_ERR_NOENT");

    const char *path = "ack_test_noent.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_ack(db, 999, 1), QDB_ERR_NOENT);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: ack pending message (never popped)
 * ---------------------------------------------------------------------- */

static void test_ack_pending_msg(void)
{
    test_begin("ack PENDING message (not leased) returns QDB_ERR_NOENT");

    const char *path = "ack_test_pending.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "hi", 2), QDB_OK);
    /* Message id=1 exists but is PENDING, not LEASED */
    ASSERT_EQ(qdb_ack(db, 1, 1), QDB_ERR_NOENT);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: ack with wrong lease_id
 * ---------------------------------------------------------------------- */

static void test_ack_wrong_lease_id(void)
{
    test_begin("ack with wrong lease_id returns QDB_ERR_INVAL");

    const char *path = "ack_test_wronglease.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t msg = {0};
    ASSERT_EQ(push_and_pop(db, "q", "data", 4, &msg), QDB_OK);

    uint64_t wrong_lease = msg.lease_id + 9999u;
    ASSERT_EQ(qdb_ack(db, msg.id, wrong_lease), QDB_ERR_INVAL);

    /* Message must still be LEASED after the failed ack */
    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_LEASED);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: valid ack
 * ---------------------------------------------------------------------- */

static void test_ack_valid(void)
{
    test_begin("valid push → pop → ack returns QDB_OK");

    const char *path = "ack_test_valid.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t msg = {0};
    ASSERT_EQ(push_and_pop(db, "q", "hello", 5, &msg), QDB_OK);

    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: ack twice (second ack returns NOENT)
 * ---------------------------------------------------------------------- */

static void test_ack_twice(void)
{
    test_begin("ack twice: second ack returns QDB_ERR_NOENT");

    const char *path = "ack_test_twice.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t msg = {0};
    ASSERT_EQ(push_and_pop(db, "q", "x", 1, &msg), QDB_OK);

    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_ERR_NOENT);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: in-memory state transitions after ack
 * ---------------------------------------------------------------------- */

static void test_ack_state_transitions(void)
{
    test_begin("ack: message→ACKED, leased_count--, acked_count++, lease removed");

    const char *path = "ack_test_state.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);

    qdb_msg_t m1 = {0}, m2 = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m1), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "q", &m2), QDB_OK);

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->leased_count, (uint32_t)2);
    ASSERT_EQ(q->acked_count,  (uint32_t)0);

    ASSERT_EQ(qdb_ack(db, m1.id, m1.lease_id), QDB_OK);

    ASSERT_EQ(q->leased_count, (uint32_t)1);
    ASSERT_EQ(q->acked_count,  (uint32_t)1);

    struct qdb__msg *msg = qdb__msg_get(db->state, m1.id);
    ASSERT_NOTNULL(msg);
    ASSERT_EQ((int)msg->state, (int)QDB_MSG_STATE_ACKED);
    ASSERT_EQ(msg->lease_id, (uint64_t)0);
    ASSERT_EQ(msg->lease_expiry_us, (uint64_t)0);

    /* Lease entry must be gone from the lease table */
    ASSERT_NULL(qdb__lease_get(db->state, m1.lease_id));

    /* m2 is still leased */
    struct qdb__msg *msg2 = qdb__msg_get(db->state, m2.id);
    ASSERT_NOTNULL(msg2);
    ASSERT_EQ((int)msg2->state, (int)QDB_MSG_STATE_LEASED);

    qdb_msg_free(&m1);
    qdb_msg_free(&m2);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: no message resurrection after reopen
 * ---------------------------------------------------------------------- */

static void test_ack_no_resurrection(void)
{
    test_begin("push → pop → ack → reopen: message never reappears");

    const char *path = "ack_test_resurr.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "bye", 3), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    qdb_close(db);

    /* Reopen — queue must be completely empty */
    db = qdb_open(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_ERR_EMPTY);

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)0);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);
    ASSERT_EQ(q->acked_count,   (uint32_t)1);

    /* Replayed message is ACKED in the message table */
    struct qdb__msg *m = qdb__msg_get(db->state, 1);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_ACKED);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: ack does not affect other messages in the same queue
 * ---------------------------------------------------------------------- */

static void test_ack_fifo_preserved(void)
{
    test_begin("ack first pop: remaining pending messages unaffected");

    const char *path = "ack_test_fifo.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    for (int i = 0; i < 5; i++) {
        char c = (char)('a' + i);
        ASSERT_EQ(qdb_push(db, "q", &c, 1), QDB_OK);
    }

    /* Pop and ack the first two */
    qdb_msg_t m = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    /* Remaining 3 messages must pop in original order */
    for (int i = 2; i < 5; i++) {
        ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
        ASSERT_EQ(((const char *)m.data)[0], (char)('a' + i));
        qdb_msg_free(&m);
    }

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: ack in one queue does not affect another queue
 * ---------------------------------------------------------------------- */

static void test_ack_multiple_queues(void)
{
    test_begin("ack in one queue does not affect messages in another queue");

    const char *path = "ack_test_multi.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "alpha", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",  "b", 1), QDB_OK);

    qdb_msg_t ma = {0}, mb = {0};
    ASSERT_EQ(qdb_pop(db, "alpha", &ma), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "beta",  &mb), QDB_OK);

    ASSERT_EQ(qdb_ack(db, ma.id, ma.lease_id), QDB_OK);
    qdb_msg_free(&ma);

    /* beta is unaffected */
    struct qdb__queue *qb = qdb__queue_get(db->state, "beta", 4);
    ASSERT_NOTNULL(qb);
    ASSERT_EQ(qb->leased_count, (uint32_t)1);
    ASSERT_EQ(qb->acked_count,  (uint32_t)0);

    ASSERT_EQ(qdb_ack(db, mb.id, mb.lease_id), QDB_OK);
    qdb_msg_free(&mb);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: log_end_offset advances by one ack record size after ack
 * ---------------------------------------------------------------------- */

static void test_ack_log_end_advances(void)
{
    test_begin("each ack advances log_end_offset by one ack record size");

    const char *path = "ack_test_logend.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t m = {0};
    ASSERT_EQ(push_and_pop(db, "q", "x", 1, &m), QDB_OK);

    /* RT_MSG_ACK record: header(9) + payload(16) + commit_marker(1) = 26 bytes */
    uint64_t expected_step = QDB_REC_HDR_SIZE + QDB_PAYLOAD_ACK_SIZE + 1u;
    uint64_t before = db->log_end_offset;

    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    ASSERT_EQ(db->log_end_offset, before + expected_step);

    qdb_msg_free(&m);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: full push → pop → ack → reopen cycle
 * ---------------------------------------------------------------------- */

static void test_ack_full_cycle(void)
{
    test_begin("full push → pop → ack → reopen: log and state consistent");

    const char *path = "ack_test_cycle.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    /* Push three messages */
    ASSERT_EQ(qdb_push(db, "jobs", "task1", 5), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "task2", 5), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "task3", 5), QDB_OK);

    /* Pop and ack the first two */
    qdb_msg_t m = {0};
    ASSERT_EQ(qdb_pop(db, "jobs", &m), QDB_OK);
    ASSERT_EQ(memcmp(m.data, "task1", 5), 0);
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "jobs", &m), QDB_OK);
    ASSERT_EQ(memcmp(m.data, "task2", 5), 0);
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    qdb_close(db);

    /* Reopen — only task3 should be pending */
    db = qdb_open(path);
    ASSERT_NOTNULL(db);

    struct qdb__queue *q = qdb__queue_get(db->state, "jobs", 4);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)1);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);
    ASSERT_EQ(q->acked_count,   (uint32_t)2);

    ASSERT_EQ(qdb_pop(db, "jobs", &m), QDB_OK);
    ASSERT_EQ(memcmp(m.data, "task3", 5), 0);
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "jobs", &m), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: large multi-queue scenario
 * ---------------------------------------------------------------------- */

static void test_ack_large_scenario(void)
{
    test_begin("push 20 → pop 20 → ack all 20 → reopen: queue empty");

    const char *path = "ack_test_large.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    const int N = 20;
    for (int i = 0; i < N; i++) {
        char buf[4];
        buf[0] = (char)(i & 0xFF); buf[1] = 0; buf[2] = 0; buf[3] = 0;
        ASSERT_EQ(qdb_push(db, "bulk", buf, 1), QDB_OK);
    }

    qdb_msg_t msgs[20];
    for (int i = 0; i < N; i++) {
        msgs[i] = (qdb_msg_t){0};
        ASSERT_EQ(qdb_pop(db, "bulk", &msgs[i]), QDB_OK);
        ASSERT_EQ(msgs[i].id, (uint64_t)(i + 1));
    }

    ASSERT_EQ(qdb_pop(db, "bulk", &msgs[0]), QDB_ERR_EMPTY); /* already all popped */

    for (int i = 0; i < N; i++) {
        ASSERT_EQ(qdb_ack(db, msgs[i].id, msgs[i].lease_id), QDB_OK);
        qdb_msg_free(&msgs[i]);
    }

    qdb_close(db);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);

    struct qdb__queue *q = qdb__queue_get(db->state, "bulk", 4);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)0);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);
    ASSERT_EQ(q->acked_count,   (uint32_t)N);

    qdb_msg_t m = {0};
    ASSERT_EQ(qdb_pop(db, "bulk", &m), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: interleaved push / pop / ack across multiple queues after reopen
 * ---------------------------------------------------------------------- */

static void test_ack_multiqueue_reopen(void)
{
    test_begin("multi-queue push/pop/ack, close, reopen: each queue consistent");

    const char *path = "ack_test_mqreopen.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    /* Alpha: push 3, pop 3, ack 2 */
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(qdb_push(db, "alpha", "a", 1), QDB_OK);
    }
    qdb_msg_t ma[3] = {{0}, {0}, {0}};
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(qdb_pop(db, "alpha", &ma[i]), QDB_OK);
    }
    ASSERT_EQ(qdb_ack(db, ma[0].id, ma[0].lease_id), QDB_OK);
    ASSERT_EQ(qdb_ack(db, ma[1].id, ma[1].lease_id), QDB_OK);
    /* ma[2] not acked — still leased */

    /* Beta: push 2, pop 1, ack 1 */
    ASSERT_EQ(qdb_push(db, "beta", "b1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta", "b2", 2), QDB_OK);
    qdb_msg_t mb = {0};
    ASSERT_EQ(qdb_pop(db, "beta", &mb), QDB_OK);
    ASSERT_EQ(qdb_ack(db, mb.id, mb.lease_id), QDB_OK);
    qdb_msg_free(&mb);

    for (int i = 0; i < 3; i++) { qdb_msg_free(&ma[i]); }

    qdb_close(db);
    db = qdb_open(path);
    ASSERT_NOTNULL(db);

    /* Alpha: 2 acked, 1 leased, 0 pending */
    struct qdb__queue *qa = qdb__queue_get(db->state, "alpha", 5);
    ASSERT_NOTNULL(qa);
    ASSERT_EQ(qa->pending_count, (uint32_t)0);
    ASSERT_EQ(qa->leased_count,  (uint32_t)1);
    ASSERT_EQ(qa->acked_count,   (uint32_t)2);

    /* Beta: 1 acked, 0 leased, 1 pending */
    struct qdb__queue *qb = qdb__queue_get(db->state, "beta", 4);
    ASSERT_NOTNULL(qb);
    ASSERT_EQ(qb->pending_count, (uint32_t)1);
    ASSERT_EQ(qb->leased_count,  (uint32_t)0);
    ASSERT_EQ(qb->acked_count,   (uint32_t)1);

    /* The still-leased alpha message can be acked in the new session */
    struct qdb__msg *leased = NULL;
    for (uint64_t id = 1; id <= 10; id++) {
        struct qdb__msg *candidate = qdb__msg_get(db->state, id);
        if (candidate && candidate->state == QDB_MSG_STATE_LEASED
                && strcmp(candidate->queue_name, "alpha") == 0) {
            leased = candidate;
            break;
        }
    }
    ASSERT_NOTNULL(leased);
    ASSERT_EQ(qdb_ack(db, leased->id, leased->lease_id), QDB_OK);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: I/O failure on ack write — no state change
 * ---------------------------------------------------------------------- */

#if !defined(_WIN32)
static void test_ack_io_failure_no_state_change(void)
{
    test_begin("I/O failure on ack write: in-memory state unchanged");

    const char *path = "ack_test_iofail.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t msg = {0};
    ASSERT_EQ(push_and_pop(db, "q", "payload", 7, &msg), QDB_OK);

    uint64_t end_before = db->log_end_offset;

    struct qdb__msg *m_before = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m_before);
    ASSERT_EQ((int)m_before->state, (int)QDB_MSG_STATE_LEASED);

    /* Close the QDB file handle so the ack write fails. */
    (void)qdb_test_close_fd(db->fd);

    int rc = qdb_ack(db, msg.id, msg.lease_id);
    ASSERT_NE(rc, QDB_OK);

    /* No state must have changed */
    ASSERT_EQ(db->log_end_offset, end_before);
    ASSERT_EQ((int)m_before->state, (int)QDB_MSG_STATE_LEASED);
    ASSERT_NOTNULL(qdb__lease_get(db->state, msg.lease_id));

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->leased_count, (uint32_t)1);
    ASSERT_EQ(q->acked_count,  (uint32_t)0);

    qdb_msg_free(&msg);
    db->fd = QDB__INVALID_FD;
    qdb_close(db);
    cleanup(path);
    test_end();
}
#endif /* !_WIN32 */

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB ack tests\n");
    printf("=============\n");

    test_ack_invalid_args();
    test_ack_nonexistent();
    test_ack_pending_msg();
    test_ack_wrong_lease_id();
    test_ack_valid();
    test_ack_twice();
    test_ack_state_transitions();
    test_ack_no_resurrection();
    test_ack_fifo_preserved();
    test_ack_multiple_queues();
    test_ack_log_end_advances();
    test_ack_full_cycle();
    test_ack_large_scenario();
    test_ack_multiqueue_reopen();
#if !defined(_WIN32)
    test_ack_io_failure_no_state_change();
#endif

    printf("=============\n");
    printf("Results: %d/%d passed", g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }

    printf("\n");
    return EXIT_SUCCESS;
}
