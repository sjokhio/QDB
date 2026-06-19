/*
 * test_pop_any.c — qdb_pop_any() tests
 *
 * Exercises input validation, global FIFO ordering across queues, lease
 * assignment, state transitions, ack/nack/expire interactions, and
 * interleaving with qdb_pop().
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

static void push_str(qdb_t *db, const char *queue, const char *data)
{
    int rc = qdb_push(db, queue, data, strlen(data));
    if (rc != QDB_OK) {
        fprintf(stderr, "push_str failed: %s\n", qdb_errmsg(rc));
    }
}

static void force_expire(qdb_t *db, uint64_t lease_id)
{
    struct qdb__lease *l = qdb__lease_get(db->state, lease_id);
    if (l) {
        l->expiry_us = 1; /* epoch + 1 µs — always in the past */
    }
}

/* -------------------------------------------------------------------------
 * Test 1: NULL argument validation
 * ---------------------------------------------------------------------- */

static void test_null_args(void)
{
    test_begin("null args: NULL db or out_msg return QDB_ERR_INVAL");

    const char *path = "pop_any_inval.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t msg = {0};

    ASSERT_EQ(qdb_pop_any(NULL, &msg), QDB_ERR_INVAL);
    ASSERT_EQ(qdb_pop_any(db,   NULL), QDB_ERR_INVAL);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 2: empty database
 * ---------------------------------------------------------------------- */

static void test_empty_db(void)
{
    test_begin("empty db: returns QDB_ERR_EMPTY");

    const char *path = "pop_any_empty.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 3: single queue — data and queue name are correct
 * ---------------------------------------------------------------------- */

static void test_single_queue(void)
{
    test_begin("single queue: data and queue name populated correctly");

    const char *path = "pop_any_single.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "work", "hello");

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_NOTNULL(msg.queue);
    ASSERT_EQ(strcmp(msg.queue, "work"), 0);
    ASSERT_EQ(msg.len, (size_t)5);
    ASSERT_EQ(memcmp(msg.data, "hello", 5), 0);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 4: two queues — out.queue identifies the correct source queue
 * ---------------------------------------------------------------------- */

static void test_queue_name_field(void)
{
    test_begin("two queues: out_msg->queue identifies source queue");

    const char *path = "pop_any_qname.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    /* push to "alpha" first so it holds the globally oldest msg_id */
    push_str(db, "alpha", "A");
    push_str(db, "beta",  "B");

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_NOTNULL(msg.queue);
    ASSERT_EQ(strcmp(msg.queue, "alpha"), 0);
    ASSERT_EQ(msg.len, (size_t)1);
    ASSERT_EQ(((char *)msg.data)[0], 'A');
    qdb_msg_free(&msg);

    /* second pop_any should get "beta" */
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_EQ(strcmp(msg.queue, "beta"), 0);
    qdb_msg_free(&msg);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 5: global push order preserved across four interleaved pushes
 * ---------------------------------------------------------------------- */

static void test_global_push_order(void)
{
    test_begin("global push order: 4 interleaved pushes delivered in ID order");

    const char *path = "pop_any_order.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    /* push A→x, B→y, C→x, D→y in that order */
    push_str(db, "x", "A");
    push_str(db, "y", "B");
    push_str(db, "x", "C");
    push_str(db, "y", "D");

    /* pop_any must deliver A, B, C, D in that order */
    qdb_msg_t msg = {0};
    uint64_t prev_id = 0;

    const char *expected[] = { "A", "B", "C", "D" };
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
        ASSERT(msg.id > prev_id);
        ASSERT_EQ(memcmp(msg.data, expected[i], 1), 0);
        prev_id = msg.id;
        qdb_ack(db, msg.id, msg.lease_id);
        qdb_msg_free(&msg);
    }

    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 6: all messages leased — returns QDB_ERR_EMPTY
 * ---------------------------------------------------------------------- */

static void test_all_leased_empty(void)
{
    test_begin("all leased: returns QDB_ERR_EMPTY");

    const char *path = "pop_any_leased.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "q1", "x");
    push_str(db, "q2", "y");

    qdb_msg_t m1 = {0}, m2 = {0};
    ASSERT_EQ(qdb_pop_any(db, &m1), QDB_OK);
    ASSERT_EQ(qdb_pop_any(db, &m2), QDB_OK);

    /* both leased — no more pending */
    qdb_msg_t m3 = {0};
    ASSERT_EQ(qdb_pop_any(db, &m3), QDB_ERR_EMPTY);

    qdb_ack(db, m1.id, m1.lease_id);
    qdb_ack(db, m2.id, m2.lease_id);
    qdb_msg_free(&m1);
    qdb_msg_free(&m2);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 7: drain all messages from multiple queues
 * ---------------------------------------------------------------------- */

static void test_drain_all(void)
{
    test_begin("drain all: 3 queues x 3 messages each drained in order");

    const char *path = "pop_any_drain.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    const char *queues[] = { "q1", "q2", "q3" };
    for (int i = 0; i < 9; i++) {
        push_str(db, queues[i % 3], "x");
    }

    int count = 0;
    uint64_t prev_id = 0;
    qdb_msg_t msg = {0};
    while (qdb_pop_any(db, &msg) == QDB_OK) {
        ASSERT(msg.id > prev_id);
        prev_id = msg.id;
        qdb_ack(db, msg.id, msg.lease_id);
        qdb_msg_free(&msg);
        count++;
    }
    ASSERT_EQ(count, 9);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 8: ack after pop_any — second pop_any returns QDB_ERR_EMPTY
 * ---------------------------------------------------------------------- */

static void test_ack_roundtrip(void)
{
    test_begin("ack roundtrip: pop_any + ack empties the queue");

    const char *path = "pop_any_ack.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "jobs", "payload");

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 9: nack sends message to tail — not back to head
 * ---------------------------------------------------------------------- */

static void test_nack_goes_to_tail(void)
{
    test_begin("nack tail semantics: nacked msg goes behind newer messages");

    const char *path = "pop_any_nack.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "q", "first");
    push_str(db, "q", "second");

    /* pop "first" then nack it */
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_EQ(memcmp(msg.data, "first", 5), 0);
    uint64_t first_id    = msg.id;
    uint64_t first_lease = msg.lease_id;
    qdb_msg_free(&msg);
    ASSERT_EQ(qdb_nack(db, first_id, first_lease), QDB_OK);

    /* next pop_any should return "second" */
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_EQ(memcmp(msg.data, "second", 6), 0);
    qdb_ack(db, msg.id, msg.lease_id);
    qdb_msg_free(&msg);

    /* then "first" (now at tail) */
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_EQ(msg.id, first_id);
    ASSERT_EQ(msg.retry_count, (uint32_t)1);
    qdb_ack(db, msg.id, msg.lease_id);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 10: expired lease is re-queued; pop_any picks it up
 * ---------------------------------------------------------------------- */

static void test_after_expire(void)
{
    test_begin("expire: expired lease re-queued; pop_any succeeds after process");

    const char *path = "pop_any_expire.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "q", "msg");

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    uint64_t lease_id = msg.lease_id;
    qdb_msg_free(&msg);

    /* force the lease into the past without sleeping */
    force_expire(db, lease_id);

    /* process_expired_leases returns the message to pending */
    ASSERT(qdb_process_expired_leases(db) >= 0);

    /* pop_any now finds it again */
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_EQ(msg.retry_count, (uint32_t)1);
    qdb_ack(db, msg.id, msg.lease_id);
    qdb_msg_free(&msg);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 11: interleaved with qdb_pop — no double delivery
 * ---------------------------------------------------------------------- */

static void test_interleaved_with_pop(void)
{
    test_begin("interleaved: mix of qdb_pop and qdb_pop_any, no double delivery");

    const char *path = "pop_any_interleave.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "jobs", "J1");
    push_str(db, "jobs", "J2");
    push_str(db, "jobs", "J3");

    qdb_msg_t m1 = {0}, m2 = {0}, m3 = {0};
    ASSERT_EQ(qdb_pop(db, "jobs", &m1), QDB_OK);
    ASSERT_EQ(qdb_pop_any(db,     &m2), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "jobs", &m3), QDB_OK);

    /* all three messages must be different */
    ASSERT_NE(m1.id, m2.id);
    ASSERT_NE(m2.id, m3.id);
    ASSERT_NE(m1.id, m3.id);

    /* queue is now empty */
    qdb_msg_t m4 = {0};
    ASSERT_EQ(qdb_pop_any(db, &m4),      QDB_ERR_EMPTY);
    ASSERT_EQ(qdb_pop(db, "jobs", &m4),  QDB_ERR_EMPTY);

    qdb_ack(db, m1.id, m1.lease_id);
    qdb_ack(db, m2.id, m2.lease_id);
    qdb_ack(db, m3.id, m3.lease_id);
    qdb_msg_free(&m1);
    qdb_msg_free(&m2);
    qdb_msg_free(&m3);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 12: cross-queue ordering — IDs monotonically increase across drain
 * ---------------------------------------------------------------------- */

static void test_cross_queue_ordering(void)
{
    test_begin("cross-queue ordering: IDs strictly increase across all queues");

    const char *path = "pop_any_cross.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    /* interleave pushes to 3 queues */
    push_str(db, "a", "1");
    push_str(db, "b", "2");
    push_str(db, "c", "3");
    push_str(db, "a", "4");
    push_str(db, "b", "5");
    push_str(db, "c", "6");

    uint64_t prev_id = 0;
    int count = 0;
    qdb_msg_t msg = {0};
    while (qdb_pop_any(db, &msg) == QDB_OK) {
        ASSERT(msg.id > prev_id);
        prev_id = msg.id;
        qdb_ack(db, msg.id, msg.lease_id);
        qdb_msg_free(&msg);
        count++;
    }
    ASSERT_EQ(count, 6);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 13: corrupt pending_head returns QDB_ERR_CORRUPT
 * ---------------------------------------------------------------------- */

static void test_corrupt_pending_head(void)
{
    test_begin("corrupt pending_head: qdb_pop_any returns QDB_ERR_CORRUPT");

    const char *path = "pop_any_corrupt.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "q", "msg");

    /* Reach into the queue and redirect pending_head to a nonexistent msg_id. */
    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT(q->pending_count > 0);
    q->pending_head = (uint64_t)0xDEADBEEF;   /* no such message in the hash */

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_ERR_CORRUPT);

    /* Restore before close so the destructor doesn't trip on the bad state. */
    q->pending_head = 0;
    q->pending_count = 0;

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 14: lease is in lease_buckets after successful pop_any
 * ---------------------------------------------------------------------- */

static void test_lease_in_bucket(void)
{
    test_begin("lease in bucket: pop_any inserts lease into lease_buckets");

    const char *path = "pop_any_lease_bucket.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "q", "data");

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);

    /* Verify the lease is reachable via qdb__lease_get(). */
    struct qdb__lease *l = qdb__lease_get(db->state, msg.lease_id);
    ASSERT_NOTNULL(l);
    ASSERT_EQ(l->lease_id,  msg.lease_id);
    ASSERT_EQ(l->msg_id,    msg.id);

    qdb_ack(db, msg.id, msg.lease_id);
    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 15: qdb_msg_free() zeroes retry_count
 * ---------------------------------------------------------------------- */

static void test_msg_free_zeros_retry_count(void)
{
    test_begin("qdb_msg_free: retry_count reset to zero");

    const char *path = "pop_any_free_retry.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    push_str(db, "q", "x");

    /* Pop then nack to increment retry_count. */
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    uint64_t mid = msg.id, lid = msg.lease_id;
    qdb_msg_free(&msg);
    ASSERT_EQ(qdb_nack(db, mid, lid), QDB_OK);

    /* Pop again — retry_count should be 1. */
    ASSERT_EQ(qdb_pop_any(db, &msg), QDB_OK);
    ASSERT_EQ(msg.retry_count, (uint32_t)1);

    /* After free, all fields including retry_count must be zero. */
    qdb_msg_free(&msg);
    ASSERT_EQ(msg.id,          (uint64_t)0);
    ASSERT_EQ(msg.lease_id,    (uint64_t)0);
    ASSERT_NULL(msg.queue);
    ASSERT_NULL(msg.data);
    ASSERT_EQ(msg.len,         (size_t)0);
    ASSERT_EQ(msg.retry_count, (uint32_t)0);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("qdb_pop_any() tests\n");

    test_null_args();
    test_empty_db();
    test_single_queue();
    test_queue_name_field();
    test_global_push_order();
    test_all_leased_empty();
    test_drain_all();
    test_ack_roundtrip();
    test_nack_goes_to_tail();
    test_after_expire();
    test_interleaved_with_pop();
    test_cross_queue_ordering();
    test_corrupt_pending_head();
    test_lease_in_bucket();
    test_msg_free_zeros_retry_count();

    printf("\n%d tests run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
