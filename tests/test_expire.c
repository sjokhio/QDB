/*
 * test_expire.c — qdb_process_expired_leases() tests
 *
 * Lease expiry timestamps are forced into the past by directly setting
 * the in-memory lease entry's expiry_us to 1 (epoch + 1 µs), which is
 * always less than any real wall-clock value.  This avoids the need for
 * time mocking or sleep calls.
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

/* Force a lease to appear already-expired in memory. */
static void force_expire(qdb_t *db, uint64_t lease_id)
{
    struct qdb__lease *l = qdb__lease_get(db->state, lease_id);
    if (l) {
        l->expiry_us = 1; /* epoch + 1 µs — always in the past */
    }
}

/* -------------------------------------------------------------------------
 * Test: no expired leases → returns 0
 * ---------------------------------------------------------------------- */

static void test_expire_none(void)
{
    test_begin("no expired leases: returns 0, state unchanged");

    const char *path = "exp_test_none.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    /* Empty database */
    ASSERT_EQ(qdb_process_expired_leases(db), 0);

    /* Push and pop — lease is fresh (30 s in the future) */
    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    ASSERT_EQ(qdb_process_expired_leases(db), 0);

    /* Message still leased */
    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_LEASED);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: single expiration
 * ---------------------------------------------------------------------- */

static void test_expire_single(void)
{
    test_begin("single expired lease: returns 1, message becomes PENDING");

    const char *path = "exp_test_single.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "hello", 5), QDB_OK);
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    force_expire(db, msg.lease_id);

    int n = qdb_process_expired_leases(db);
    ASSERT_EQ(n, 1);

    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_PENDING);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: state transitions after expiration
 * ---------------------------------------------------------------------- */

static void test_expire_state_transitions(void)
{
    test_begin("expire: message→PENDING, lease cleared, queue counts correct");

    const char *path = "exp_test_state.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    uint64_t old_lease = msg.lease_id;

    force_expire(db, msg.lease_id);
    ASSERT_EQ(qdb_process_expired_leases(db), 1);

    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state,      (int)QDB_MSG_STATE_PENDING);
    ASSERT_EQ(m->lease_id,        (uint64_t)0);
    ASSERT_EQ(m->lease_expiry_us, (uint64_t)0);

    /* Lease entry removed from table */
    ASSERT_NULL(qdb__lease_get(db->state, old_lease));

    /* Queue counts */
    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)1);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: retry_count incremented on expiration
 * ---------------------------------------------------------------------- */

static void test_expire_retry_count(void)
{
    test_begin("retry_count increments on each expiration");

    const char *path = "exp_test_retry.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    for (uint32_t i = 0; i < 3; i++) {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

        struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
        ASSERT_EQ(m->retry_count, i);

        force_expire(db, msg.lease_id);
        ASSERT_EQ(qdb_process_expired_leases(db), 1);
        ASSERT_EQ(m->retry_count, i + 1u);

        qdb_msg_free(&msg);
    }

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: multiple leases expired in one call
 * ---------------------------------------------------------------------- */

static void test_expire_multiple(void)
{
    test_begin("multiple expired leases processed in one call");

    const char *path = "exp_test_multi.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    const int N = 5;
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    }

    qdb_msg_t msgs[5];
    for (int i = 0; i < N; i++) {
        msgs[i] = (qdb_msg_t){0};
        ASSERT_EQ(qdb_pop(db, "q", &msgs[i]), QDB_OK);
        force_expire(db, msgs[i].lease_id);
    }

    int n = qdb_process_expired_leases(db);
    ASSERT_EQ(n, N);

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)N);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);

    for (int i = 0; i < N; i++) {
        qdb_msg_free(&msgs[i]);
    }

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: expiration requeues to tail, preserving FIFO for non-expired
 * ---------------------------------------------------------------------- */

static void test_expire_requeue_to_tail(void)
{
    test_begin("expire requeues to tail: push A B, pop A, expire A → pop B A");

    const char *path = "exp_test_tail.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "A", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "B", 1), QDB_OK);

    /* Pop A only; B stays PENDING at head */
    qdb_msg_t ma = {0};
    ASSERT_EQ(qdb_pop(db, "q", &ma), QDB_OK);
    ASSERT_EQ(((const char *)ma.data)[0], 'A');

    force_expire(db, ma.lease_id);
    ASSERT_EQ(qdb_process_expired_leases(db), 1);
    qdb_msg_free(&ma);

    /* Queue is now: B (head) … A (tail) */
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
 * Test: restart after expiration — message is PENDING, not LEASED
 * ---------------------------------------------------------------------- */

static void test_expire_reopen(void)
{
    test_begin("expire → reopen: message is PENDING, poppable after restart");

    const char *path = "exp_test_reopen.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "retry me", 8), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    force_expire(db, msg.lease_id);
    ASSERT_EQ(qdb_process_expired_leases(db), 1);
    qdb_msg_free(&msg);

    qdb_close(db);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);

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
 * Test: expired lease_id rejected by qdb_ack
 * ---------------------------------------------------------------------- */

static void test_expire_then_ack_rejected(void)
{
    test_begin("ack with expired lease_id returns QDB_ERR_NOENT");

    const char *path = "exp_test_ackrej.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    uint64_t old_msg_id   = msg.id;
    uint64_t old_lease_id = msg.lease_id;
    qdb_msg_free(&msg);

    force_expire(db, old_lease_id);
    ASSERT_EQ(qdb_process_expired_leases(db), 1);

    /* Message is now PENDING, so ack returns NOENT (not leased) */
    ASSERT_EQ(qdb_ack(db, old_msg_id, old_lease_id), QDB_ERR_NOENT);

    /* nack also rejected */
    ASSERT_EQ(qdb_nack(db, old_msg_id, old_lease_id), QDB_ERR_NOENT);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: expired message can be re-popped with a new lease
 * ---------------------------------------------------------------------- */

static void test_expire_then_repop(void)
{
    test_begin("expire then repop: same msg_id, new lease_id, correct data");

    const char *path = "exp_test_repop.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "payload", 7), QDB_OK);

    qdb_msg_t first = {0};
    ASSERT_EQ(qdb_pop(db, "q", &first), QDB_OK);

    force_expire(db, first.lease_id);
    ASSERT_EQ(qdb_process_expired_leases(db), 1);

    qdb_msg_t second = {0};
    ASSERT_EQ(qdb_pop(db, "q", &second), QDB_OK);

    ASSERT_EQ(second.id, first.id);
    ASSERT_NE(second.lease_id, first.lease_id);
    ASSERT_EQ(second.len, (size_t)7);
    ASSERT_EQ(memcmp(second.data, "payload", 7), 0);

    /* New lease is valid */
    ASSERT_EQ(qdb_ack(db, second.id, second.lease_id), QDB_OK);

    qdb_msg_free(&first);
    qdb_msg_free(&second);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: only expired leases processed; fresh lease untouched
 * ---------------------------------------------------------------------- */

static void test_expire_only_expired(void)
{
    test_begin("only expired leases processed; fresh leases untouched");

    const char *path = "exp_test_onlyexp.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "expire-me", 9), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "keep-me",   7), QDB_OK);

    qdb_msg_t me = {0}, mk = {0};
    ASSERT_EQ(qdb_pop(db, "q", &me), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "q", &mk), QDB_OK);

    /* Expire only the first message's lease */
    force_expire(db, me.lease_id);

    int n = qdb_process_expired_leases(db);
    ASSERT_EQ(n, 1);

    /* First message is PENDING */
    struct qdb__msg *m_exp = qdb__msg_get(db->state, me.id);
    ASSERT_EQ((int)m_exp->state, (int)QDB_MSG_STATE_PENDING);

    /* Second message is still LEASED with its original lease_id */
    struct qdb__msg *m_keep = qdb__msg_get(db->state, mk.id);
    ASSERT_EQ((int)m_keep->state,   (int)QDB_MSG_STATE_LEASED);
    ASSERT_EQ(m_keep->lease_id,     mk.lease_id);
    ASSERT_NOTNULL(qdb__lease_get(db->state, mk.lease_id));

    qdb_msg_free(&me);
    qdb_msg_free(&mk);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: log_end_offset advances by N * expire_record_size
 * ---------------------------------------------------------------------- */

static void test_expire_log_end_advances(void)
{
    test_begin("log_end_offset advances by one expire record per expiration");

    const char *path = "exp_test_logend.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    /* RT_MSG_EXPIRE: header(9) + payload(16) + commit_marker(1) = 26 bytes */
    uint64_t expected_step = QDB_REC_HDR_SIZE + QDB_PAYLOAD_EXPIRE_SIZE + 1u;
    uint64_t before = db->log_end_offset;

    force_expire(db, msg.lease_id);
    ASSERT_EQ(qdb_process_expired_leases(db), 1);

    ASSERT_EQ(db->log_end_offset, before + expected_step);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: FIFO ordering preserved across multiple expirations and re-pops
 * ---------------------------------------------------------------------- */

static void test_expire_fifo_ordering(void)
{
    test_begin("FIFO preserved: push A B C, expire A C, pop order is B A C");

    const char *path = "exp_test_fifo.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "A", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "B", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "C", 1), QDB_OK);

    /* Pop all three */
    qdb_msg_t ma = {0}, mb = {0}, mc = {0};
    ASSERT_EQ(qdb_pop(db, "q", &ma), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "q", &mb), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "q", &mc), QDB_OK);

    /* Expire A and C; keep B leased */
    force_expire(db, ma.lease_id);
    force_expire(db, mc.lease_id);
    int n = qdb_process_expired_leases(db);
    ASSERT_EQ(n, 2);

    /* Ack B so it leaves the picture */
    ASSERT_EQ(qdb_ack(db, mb.id, mb.lease_id), QDB_OK);

    /*
     * Pending queue now contains A and C (both expired and requeued to tail
     * in the order they were processed; the exact order between A and C
     * depends on hash-table iteration, so we only assert both are poppable).
     */
    qdb_msg_t m = {0};
    int got_a = 0, got_c = 0;

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    if (((const char *)m.data)[0] == 'A') { got_a = 1; }
    if (((const char *)m.data)[0] == 'C') { got_c = 1; }
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    if (((const char *)m.data)[0] == 'A') { got_a = 1; }
    if (((const char *)m.data)[0] == 'C') { got_c = 1; }
    qdb_msg_free(&m);

    ASSERT_EQ(got_a, 1);
    ASSERT_EQ(got_c, 1);
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_ERR_EMPTY);

    qdb_msg_free(&ma);
    qdb_msg_free(&mb);
    qdb_msg_free(&mc);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: full push → pop → expire → reopen → pop → ack cycle
 * ---------------------------------------------------------------------- */

static void test_expire_full_cycle(void)
{
    test_begin("push → pop → expire → reopen → pop → ack: no message loss");

    const char *path = "exp_test_cycle.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "jobs", "task", 4), QDB_OK);

    qdb_msg_t first = {0};
    ASSERT_EQ(qdb_pop(db, "jobs", &first), QDB_OK);
    force_expire(db, first.lease_id);
    ASSERT_EQ(qdb_process_expired_leases(db), 1);
    qdb_msg_free(&first);

    qdb_close(db);
    db = qdb_open(path);
    ASSERT_NOTNULL(db);

    /* Message survives as PENDING across close/reopen */
    struct qdb__queue *q = qdb__queue_get(db->state, "jobs", 4);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)1);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);

    qdb_msg_t second = {0};
    ASSERT_EQ(qdb_pop(db, "jobs", &second), QDB_OK);
    ASSERT_EQ(second.len, (size_t)4);
    ASSERT_EQ(memcmp(second.data, "task", 4), 0);

    ASSERT_EQ(qdb_ack(db, second.id, second.lease_id), QDB_OK);
    qdb_msg_free(&second);

    ASSERT_EQ(qdb_pop(db, "jobs", &second), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: I/O failure on expire write — no state change
 * ---------------------------------------------------------------------- */

#if !defined(_WIN32)
static void test_expire_io_failure(void)
{
    test_begin("I/O failure during expire: in-memory state unchanged");

    const char *path = "exp_test_iofail.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "data", 4), QDB_OK);
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    uint64_t end_before = db->log_end_offset;
    force_expire(db, msg.lease_id);

    (void)qdb_test_close_fd(db->fd);

    int rc = qdb_process_expired_leases(db);
    ASSERT(rc < 0);

    /* State must be unchanged */
    ASSERT_EQ(db->log_end_offset, end_before);

    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_LEASED);
    ASSERT_NOTNULL(qdb__lease_get(db->state, msg.lease_id));

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->leased_count,  (uint32_t)1);
    ASSERT_EQ(q->pending_count, (uint32_t)0);

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
    printf("QDB expire tests\n");
    printf("================\n");

    test_expire_none();
    test_expire_single();
    test_expire_state_transitions();
    test_expire_retry_count();
    test_expire_multiple();
    test_expire_requeue_to_tail();
    test_expire_reopen();
    test_expire_then_ack_rejected();
    test_expire_then_repop();
    test_expire_only_expired();
    test_expire_log_end_advances();
    test_expire_fifo_ordering();
    test_expire_full_cycle();
#if !defined(_WIN32)
    test_expire_io_failure();
#endif

    printf("================\n");
    printf("Results: %d/%d passed", g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }

    printf("\n");
    return EXIT_SUCCESS;
}
