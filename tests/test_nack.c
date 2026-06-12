/*
 * test_nack.c — qdb_nack() tests
 *
 * Exercises input validation, lease validation, in-memory state transitions,
 * FIFO requeue-to-tail semantics, durability across close/reopen, and the
 * guarantee that a failed disk write leaves in-memory state unchanged.
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

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

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
    char sidecar[512];
    size_t plen = strlen(path);
#if defined(_WIN32)
    DeleteFileA(path);
    if (plen + 6 < sizeof(sidecar)) {
        memcpy(sidecar, path, plen); memcpy(sidecar + plen, "-wal",  5); DeleteFileA(sidecar);
        memcpy(sidecar, path, plen); memcpy(sidecar + plen, "-lock", 6); DeleteFileA(sidecar);
    }
#else
    (void)unlink(path);
    if (plen + 6 < sizeof(sidecar)) {
        memcpy(sidecar, path, plen); memcpy(sidecar + plen, "-wal",  5); (void)unlink(sidecar);
        memcpy(sidecar, path, plen); memcpy(sidecar + plen, "-lock", 6); (void)unlink(sidecar);
    }
#endif
}

static qdb_t *open_fresh(const char *path)
{
    cleanup(path);
    return qdb_open(path);
}

/* -------------------------------------------------------------------------
 * Test: invalid arguments
 * ---------------------------------------------------------------------- */

static void test_nack_invalid_args(void)
{
    test_begin("invalid args: NULL db returns QDB_ERR_INVAL");
    ASSERT_EQ(qdb_nack(NULL, 1, 1), QDB_ERR_INVAL);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: nack nonexistent message
 * ---------------------------------------------------------------------- */

static void test_nack_nonexistent(void)
{
    test_begin("nack unknown msg_id returns QDB_ERR_NOENT");

    const char *path = "nack_test_noent.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_nack(db, 999, 1), QDB_ERR_NOENT);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: nack a PENDING message (never popped)
 * ---------------------------------------------------------------------- */

static void test_nack_pending_msg(void)
{
    test_begin("nack PENDING message (not leased) returns QDB_ERR_NOENT");

    const char *path = "nack_test_pending.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "hi", 2), QDB_OK);
    ASSERT_EQ(qdb_nack(db, 1, 1), QDB_ERR_NOENT);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: nack with wrong lease_id
 * ---------------------------------------------------------------------- */

static void test_nack_wrong_lease_id(void)
{
    test_begin("nack with wrong lease_id returns QDB_ERR_INVAL");

    const char *path = "nack_test_wronglease.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "data", 4), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    ASSERT_EQ(qdb_nack(db, msg.id, msg.lease_id + 1u), QDB_ERR_INVAL);

    /* Message must still be LEASED */
    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_LEASED);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: valid nack
 * ---------------------------------------------------------------------- */

static void test_nack_valid(void)
{
    test_begin("valid push → pop → nack returns QDB_OK");

    const char *path = "nack_test_valid.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "hello", 5), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(qdb_nack(db, msg.id, msg.lease_id), QDB_OK);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: in-memory state transitions after nack
 * ---------------------------------------------------------------------- */

static void test_nack_state_transitions(void)
{
    test_begin("nack: message→PENDING, leased_count--, lease removed, retry++");

    const char *path = "nack_test_state.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)0);
    ASSERT_EQ(q->leased_count,  (uint32_t)1);

    uint64_t old_lease = msg.lease_id;
    ASSERT_EQ(qdb_nack(db, msg.id, msg.lease_id), QDB_OK);

    /* Queue counts */
    ASSERT_EQ(q->pending_count, (uint32_t)1);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);

    /* Message state */
    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state,   (int)QDB_MSG_STATE_PENDING);
    ASSERT_EQ(m->lease_id,     (uint64_t)0);
    ASSERT_EQ(m->lease_expiry_us, (uint64_t)0);
    ASSERT_EQ(m->retry_count,  (uint32_t)1);

    /* Lease entry must be gone */
    ASSERT_NULL(qdb__lease_get(db->state, old_lease));

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: nack requeues to tail (FIFO preserved for others)
 * ---------------------------------------------------------------------- */

static void test_nack_requeue_to_tail(void)
{
    test_begin("nack requeues to tail: push A B, pop A, nack A → pop order B A");

    const char *path = "nack_test_tail.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "A", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "B", 1), QDB_OK);

    qdb_msg_t ma = {0};
    ASSERT_EQ(qdb_pop(db, "q", &ma), QDB_OK);
    ASSERT_EQ(((const char *)ma.data)[0], 'A');

    ASSERT_EQ(qdb_nack(db, ma.id, ma.lease_id), QDB_OK);
    qdb_msg_free(&ma);

    /* Queue now has B at head, A at tail */
    qdb_msg_t m = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    ASSERT_EQ(((const char *)m.data)[0], 'B');
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    ASSERT_EQ(((const char *)m.data)[0], 'A');
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: double nack (second nack sees PENDING, not LEASED)
 * ---------------------------------------------------------------------- */

static void test_nack_double(void)
{
    test_begin("double nack: second nack returns QDB_ERR_NOENT");

    const char *path = "nack_test_double.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    uint64_t id       = msg.id;
    uint64_t lease_id = msg.lease_id;
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_nack(db, id, lease_id), QDB_OK);
    /* Message is now PENDING; nacking again returns NOENT (not leased) */
    ASSERT_EQ(qdb_nack(db, id, lease_id), QDB_ERR_NOENT);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: nack then pop gets the same message back
 * ---------------------------------------------------------------------- */

static void test_nack_then_pop(void)
{
    test_begin("nack then pop: same message redelivered with new lease_id");

    const char *path = "nack_test_repop.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "payload", 7), QDB_OK);

    qdb_msg_t first = {0};
    ASSERT_EQ(qdb_pop(db, "q", &first), QDB_OK);
    ASSERT_EQ(qdb_nack(db, first.id, first.lease_id), QDB_OK);

    qdb_msg_t second = {0};
    ASSERT_EQ(qdb_pop(db, "q", &second), QDB_OK);

    /* Same message id, different lease_id */
    ASSERT_EQ(second.id, first.id);
    ASSERT_NE(second.lease_id, first.lease_id);
    ASSERT_EQ(second.len, (size_t)7);
    ASSERT_EQ(memcmp(second.data, "payload", 7), 0);

    qdb_msg_free(&first);
    qdb_msg_free(&second);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: retry_count increments on each nack
 * ---------------------------------------------------------------------- */

static void test_nack_retry_count(void)
{
    test_begin("retry_count increments on each nack cycle");

    const char *path = "nack_test_retry.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    for (uint32_t i = 0; i < 3; i++) {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

        struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
        ASSERT_NOTNULL(m);
        ASSERT_EQ(m->retry_count, i);

        ASSERT_EQ(qdb_nack(db, msg.id, msg.lease_id), QDB_OK);
        ASSERT_EQ(m->retry_count, i + 1u);

        qdb_msg_free(&msg);
    }

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: restart after nack — message is PENDING, not LEASED or ACKED
 * ---------------------------------------------------------------------- */

static void test_nack_reopen(void)
{
    test_begin("push → pop → nack → reopen: message is PENDING");

    const char *path = "nack_test_reopen.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "retry me", 8), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(qdb_nack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    qdb_close(db);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);

    /* Message must be PENDING and poppable */
    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)1);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);

    struct qdb__msg *m = qdb__msg_get(db->state, 1);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state,  (int)QDB_MSG_STATE_PENDING);
    ASSERT_EQ(m->retry_count, (uint32_t)1);

    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(memcmp(msg.data, "retry me", 8), 0);
    qdb_msg_free(&msg);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: nack then ack (via second pop)
 * ---------------------------------------------------------------------- */

static void test_nack_then_ack(void)
{
    test_begin("pop → nack → pop → ack: message permanently consumed");

    const char *path = "nack_test_thenack.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "work", 4), QDB_OK);

    qdb_msg_t m = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    ASSERT_EQ(qdb_nack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: multiple nacks with multiple queues
 * ---------------------------------------------------------------------- */

static void test_nack_multiple_queues(void)
{
    test_begin("nack in one queue does not affect another queue");

    const char *path = "nack_test_multi.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "alpha", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",  "b", 1), QDB_OK);

    qdb_msg_t ma = {0}, mb = {0};
    ASSERT_EQ(qdb_pop(db, "alpha", &ma), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "beta",  &mb), QDB_OK);

    ASSERT_EQ(qdb_nack(db, ma.id, ma.lease_id), QDB_OK);
    qdb_msg_free(&ma);

    /* beta is unaffected */
    struct qdb__queue *qb = qdb__queue_get(db->state, "beta", 4);
    ASSERT_NOTNULL(qb);
    ASSERT_EQ(qb->leased_count,  (uint32_t)1);
    ASSERT_EQ(qb->pending_count, (uint32_t)0);

    ASSERT_EQ(qdb_ack(db, mb.id, mb.lease_id), QDB_OK);
    qdb_msg_free(&mb);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: log_end_offset advances by one nack record size
 * ---------------------------------------------------------------------- */

static void test_nack_log_end_advances(void)
{
    test_begin("each nack advances log_end_offset by one nack record size");

    const char *path = "nack_test_logend.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    /* RT_MSG_NACK: header(9) + payload(16) + commit_marker(1) = 26 bytes */
    uint64_t expected_step = QDB_REC_HDR_SIZE + QDB_PAYLOAD_NACK_SIZE + 1u;
    uint64_t before = db->log_end_offset;

    ASSERT_EQ(qdb_nack(db, msg.id, msg.lease_id), QDB_OK);
    ASSERT_EQ(db->log_end_offset, before + expected_step);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: nack preserves tail ordering across multiple requeues
 * ---------------------------------------------------------------------- */

static void test_nack_tail_ordering(void)
{
    test_begin("multiple nacks: requeued messages accumulate at tail in order");

    const char *path = "nack_test_order.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    /* Push C B A in order */
    ASSERT_EQ(qdb_push(db, "q", "C", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "B", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "A", 1), QDB_OK);

    /* Pop and nack all three — they all go to tail in pop order: C, B, A */
    for (int i = 0; i < 3; i++) {
        qdb_msg_t m = {0};
        ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
        ASSERT_EQ(qdb_nack(db, m.id, m.lease_id), QDB_OK);
        qdb_msg_free(&m);
    }

    /* Original push order was C B A (IDs 1 2 3).
     * Pop order was C(1) B(2) A(3); nack order same.
     * After all three nacks, pending list is: C B A (tail-appended in pop order). */
    const char expected[] = {'C', 'B', 'A'};
    for (int i = 0; i < 3; i++) {
        qdb_msg_t m = {0};
        ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
        ASSERT_EQ(((const char *)m.data)[0], expected[i]);
        qdb_msg_free(&m);
    }

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: I/O failure on nack write — no state change
 * ---------------------------------------------------------------------- */

#if !defined(_WIN32)
static void test_nack_io_failure_no_state_change(void)
{
    test_begin("I/O failure on nack write: in-memory state unchanged");

    const char *path = "nack_test_iofail.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "payload", 7), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    uint64_t end_before = db->log_end_offset;

    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_LEASED);

    int saved_fd = (int)db->fd;
    close(saved_fd);

    int rc = qdb_nack(db, msg.id, msg.lease_id);
    ASSERT_NE(rc, QDB_OK);

    /* Nothing must have changed */
    ASSERT_EQ(db->log_end_offset, end_before);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_LEASED);
    ASSERT_NOTNULL(qdb__lease_get(db->state, msg.lease_id));

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)0);
    ASSERT_EQ(q->leased_count,  (uint32_t)1);

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
    printf("QDB nack tests\n");
    printf("==============\n");

    test_nack_invalid_args();
    test_nack_nonexistent();
    test_nack_pending_msg();
    test_nack_wrong_lease_id();
    test_nack_valid();
    test_nack_state_transitions();
    test_nack_requeue_to_tail();
    test_nack_double();
    test_nack_then_pop();
    test_nack_retry_count();
    test_nack_reopen();
    test_nack_then_ack();
    test_nack_multiple_queues();
    test_nack_log_end_advances();
    test_nack_tail_ordering();
#if !defined(_WIN32)
    test_nack_io_failure_no_state_change();
#endif

    printf("==============\n");
    printf("Results: %d/%d passed", g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }

    printf("\n");
    return EXIT_SUCCESS;
}
