/*
 * test_push.c — qdb_push() tests
 *
 * Exercises input validation, in-memory state updates, durability across
 * close/reopen, message-ID monotonicity, and the guarantee that a failed
 * disk write leaves in-memory state completely unchanged.
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

/* Queue-find callback used with qdb__state_iter_queues. */
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

static void test_push_one(void)
{
    const char *path = "push_one.qdb";
    test_begin("push one message: in-memory state correct");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    int rc = qdb_push(db, "jobs", "hello", 5);
    ASSERT_EQ(rc, QDB_OK);

    /* Message count */
    ASSERT_EQ(db->state->msg_count, 1u);

    /* next_msg_id advanced */
    ASSERT_EQ(db->next_msg_id, 2u);

    /* Message entry */
    const struct qdb__msg *m = qdb__msg_get(db->state, 1);
    ASSERT_NOTNULL(m);
    if (m) {
        ASSERT_EQ(m->state,     (qdb_msg_state_t)QDB_MSG_STATE_PENDING);
        ASSERT_EQ(m->data_len,  5u);
        ASSERT_EQ(m->lease_id,  0u);
        ASSERT_EQ(strcmp(m->queue_name, "jobs"), 0);
    }

    /* Queue */
    struct queue_find_ctx fc;
    memset(&fc, 0, sizeof(fc));
    fc.name = "jobs";
    qdb__state_iter_queues(db->state, find_queue, &fc);
    ASSERT_EQ(fc.hit,                  1);
    ASSERT_EQ(fc.found.pending_count,  1u);
    ASSERT_EQ(fc.found.pending_head,   1u);
    ASSERT_EQ(fc.found.pending_tail,   1u);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_push_many(void)
{
    const char *path = "push_many.qdb";
    const int   N    = 50;
    test_begin("push 50 messages: counts and FIFO order correct");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    for (int i = 0; i < N; i++) {
        char payload[8];
        qdb__put_u64le((uint8_t *)payload, (uint64_t)i);
        ASSERT_EQ(qdb_push(db, "q", payload, 8), QDB_OK);
    }

    ASSERT_EQ(db->state->msg_count, (uint64_t)N);
    ASSERT_EQ(db->next_msg_id,      (uint64_t)(N + 1));

    struct queue_find_ctx fc;
    memset(&fc, 0, sizeof(fc));
    fc.name = "q";
    qdb__state_iter_queues(db->state, find_queue, &fc);
    ASSERT_EQ(fc.found.pending_count, (uint32_t)N);
    ASSERT_EQ(fc.found.pending_head,  1u);              /* first pushed = head */
    ASSERT_EQ(fc.found.pending_tail,  (uint64_t)N);    /* last pushed = tail */

    /* Verify the linked list is contiguous: 1 → 2 → … → N */
    uint64_t cur = fc.found.pending_head;
    for (int i = 1; i <= N; i++) {
        const struct qdb__msg *m = qdb__msg_get(db->state, cur);
        ASSERT_NOTNULL(m);
        if (!m) { break; }
        ASSERT_EQ(m->id, (uint64_t)i);
        cur = m->next_pending;
    }
    ASSERT_EQ(cur, 0u); /* end of list */

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_push_multiple_queues(void)
{
    const char *path = "push_multiqueue.qdb";
    test_begin("push to 3 queues: each queue has independent FIFO");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "alpha", "a1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",  "b1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "alpha", "a2", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "gamma", "g1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",  "b2", 2), QDB_OK);

    ASSERT_EQ(db->state->msg_count,   5u);
    ASSERT_EQ(db->state->queue_count, 3u);

    {
        struct queue_find_ctx fc;
        memset(&fc, 0, sizeof(fc));
        fc.name = "alpha";
        qdb__state_iter_queues(db->state, find_queue, &fc);
        ASSERT_EQ(fc.found.pending_count, 2u);
        ASSERT_EQ(fc.found.pending_head,  1u);
        ASSERT_EQ(fc.found.pending_tail,  3u);
    }
    {
        struct queue_find_ctx fc;
        memset(&fc, 0, sizeof(fc));
        fc.name = "beta";
        qdb__state_iter_queues(db->state, find_queue, &fc);
        ASSERT_EQ(fc.found.pending_count, 2u);
        ASSERT_EQ(fc.found.pending_head,  2u);
        ASSERT_EQ(fc.found.pending_tail,  5u);
    }
    {
        struct queue_find_ctx fc;
        memset(&fc, 0, sizeof(fc));
        fc.name = "gamma";
        qdb__state_iter_queues(db->state, find_queue, &fc);
        ASSERT_EQ(fc.found.pending_count, 1u);
        ASSERT_EQ(fc.found.pending_head,  4u);
        ASSERT_EQ(fc.found.pending_tail,  4u);
    }

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_push_zero_length(void)
{
    const char *path = "push_zerolen.qdb";
    test_begin("push zero-length message: succeeds, data_len == 0");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* NULL data with len=0 must be accepted. */
    int rc = qdb_push(db, "q", NULL, 0);
    ASSERT_EQ(rc, QDB_OK);

    const struct qdb__msg *m = qdb__msg_get(db->state, 1);
    ASSERT_NOTNULL(m);
    if (m) {
        ASSERT_EQ(m->data_len, 0u);
        ASSERT_EQ(m->state, (qdb_msg_state_t)QDB_MSG_STATE_PENDING);
    }

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_push_invalid_queue_name(void)
{
    const char *path = "push_badname.qdb";
    test_begin("invalid queue names: QDB_ERR_INVAL returned");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* NULL queue name */
    ASSERT_EQ(qdb_push(db, NULL, "x", 1), QDB_ERR_INVAL);

    /* Empty queue name */
    ASSERT_EQ(qdb_push(db, "", "x", 1), QDB_ERR_INVAL);

    /* Name exactly at the limit is OK (255 chars). */
    {
        char name[QDB_QUEUE_NAME_MAX + 1];
        memset(name, 'x', QDB_QUEUE_NAME_MAX);
        name[QDB_QUEUE_NAME_MAX] = '\0';
        ASSERT_EQ(qdb_push(db, name, "x", 1), QDB_OK);
    }

    /* Name one byte over the limit must be rejected. */
    {
        char name[QDB_QUEUE_NAME_MAX + 2];
        memset(name, 'x', QDB_QUEUE_NAME_MAX + 1);
        name[QDB_QUEUE_NAME_MAX + 1] = '\0';
        ASSERT_EQ(qdb_push(db, name, "x", 1), QDB_ERR_INVAL);
    }

    /* No messages added for the bad calls. */
    ASSERT_EQ(db->state->msg_count, 1u); /* only the 255-char name push */

    /* NULL data with len > 0 */
    ASSERT_EQ(qdb_push(db, "q", NULL, 1), QDB_ERR_INVAL);

    /* NULL db */
    ASSERT_EQ(qdb_push(NULL, "q", "x", 1), QDB_ERR_INVAL);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_push_oversized_message(void)
{
    const char *path = "push_oversized.qdb";
    test_begin("oversized message: QDB_ERR_INVAL returned");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* len == QDB_MSG_MAX_LEN is the largest valid message. */
    /* We can't allocate 64 MiB in a unit test, so just check the boundary
     * condition using len = QDB_MSG_MAX_LEN + 1. */
    ASSERT_EQ(qdb_push(db, "q", "x", QDB_MSG_MAX_LEN + 1u), QDB_ERR_INVAL);
    ASSERT_EQ(db->state->msg_count, 0u);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_push_reopen_replay(void)
{
    const char *path = "push_reopen.qdb";
    test_begin("push then reopen: state fully reconstructed from log");
    cleanup(path);

    /* Push 5 messages in first session. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        ASSERT_EQ(qdb_push(db, "jobs",   "j1", 2), QDB_OK);
        ASSERT_EQ(qdb_push(db, "jobs",   "j2", 2), QDB_OK);
        ASSERT_EQ(qdb_push(db, "alerts", "a1", 2), QDB_OK);
        ASSERT_EQ(qdb_push(db, "jobs",   "j3", 2), QDB_OK);
        ASSERT_EQ(qdb_push(db, "alerts", "a2", 2), QDB_OK);
        qdb_close(db);
    }

    /* Reopen and verify reconstructed state. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        ASSERT_EQ(db->state->msg_count,   5u);
        ASSERT_EQ(db->state->queue_count, 2u);
        ASSERT_EQ(db->next_msg_id,        6u);

        struct queue_find_ctx fc;
        memset(&fc, 0, sizeof(fc));
        fc.name = "jobs";
        qdb__state_iter_queues(db->state, find_queue, &fc);
        ASSERT_EQ(fc.found.pending_count, 3u);
        ASSERT_EQ(fc.found.pending_head,  1u);
        ASSERT_EQ(fc.found.pending_tail,  4u);

        memset(&fc, 0, sizeof(fc));
        fc.name = "alerts";
        qdb__state_iter_queues(db->state, find_queue, &fc);
        ASSERT_EQ(fc.found.pending_count, 2u);
        ASSERT_EQ(fc.found.pending_head,  3u);
        ASSERT_EQ(fc.found.pending_tail,  5u);

        /* Verify message fields. */
        const struct qdb__msg *m1 = qdb__msg_get(db->state, 1);
        ASSERT_NOTNULL(m1);
        if (m1) {
            ASSERT_EQ(m1->data_len, 2u);
            ASSERT_EQ(m1->state, (qdb_msg_state_t)QDB_MSG_STATE_PENDING);
            ASSERT_EQ(strcmp(m1->queue_name, "jobs"), 0);
        }

        qdb_close(db);
    }
    test_end();
    cleanup(path);
}

static void test_push_msg_ids_never_reused(void)
{
    const char *path = "push_ids.qdb";
    test_begin("message IDs never reused after close/reopen");
    cleanup(path);

    /* First session: push 3 messages → IDs 1, 2, 3. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }
        ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "c", 1), QDB_OK);
        ASSERT_EQ(db->next_msg_id, 4u);
        qdb_close(db);
    }

    /* Second session: next IDs must start at 4, never recycle 1–3. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        ASSERT_EQ(db->next_msg_id, 4u);

        ASSERT_EQ(qdb_push(db, "q", "d", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "e", 1), QDB_OK);

        const struct qdb__msg *m4 = qdb__msg_get(db->state, 4);
        const struct qdb__msg *m5 = qdb__msg_get(db->state, 5);
        ASSERT_NOTNULL(m4);
        ASSERT_NOTNULL(m5);

        /* IDs 1–3 still exist in state (pushed in first session). */
        ASSERT_NOTNULL(qdb__msg_get(db->state, 1));
        ASSERT_NOTNULL(qdb__msg_get(db->state, 2));
        ASSERT_NOTNULL(qdb__msg_get(db->state, 3));

        ASSERT_EQ(db->next_msg_id, 6u);
        qdb_close(db);
    }

    /* Third session: IDs continue from 6. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }
        ASSERT_EQ(db->next_msg_id, 6u);
        qdb_close(db);
    }

    test_end();
    cleanup(path);
}

static void test_push_data_file_offset(void)
{
    const char *path = "push_offset.qdb";
    test_begin("data_file_offset points to correct bytes in file");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    const char payload[] = "TESTDATA";
    ASSERT_EQ(qdb_push(db, "q", payload, 8), QDB_OK);

    const struct qdb__msg *m = qdb__msg_get(db->state, 1);
    ASSERT_NOTNULL(m);

    if (m) {
        /* Read back the data bytes from the file offset stored in the message. */
        uint8_t rbuf[8];
        int rc = qdb__read_full(db->fd, rbuf, 8, m->data_file_offset);
        ASSERT_EQ(rc, QDB_OK);
        ASSERT_EQ(memcmp(rbuf, payload, 8), 0);
    }

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_push_append_failure_no_mutation(void)
{
    const char *path = "push_fail.qdb";
    test_begin("I/O failure on append: in-memory state unchanged");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Establish a known baseline. */
    ASSERT_EQ(qdb_push(db, "q", "baseline", 8), QDB_OK);

    uint64_t saved_msg_count   = db->state->msg_count;
    uint64_t saved_queue_count = db->state->queue_count;
    uint64_t saved_next_id     = db->next_msg_id;
    uint64_t saved_log_end     = db->log_end_offset;

    uint32_t saved_pending = 0;
    {
        struct queue_find_ctx fc;
        memset(&fc, 0, sizeof(fc));
        fc.name = "q";
        qdb__state_iter_queues(db->state, find_queue, &fc);
        saved_pending = fc.found.pending_count;
    }

    /* Force I/O failure by closing the underlying QDB file handle. */
    (void)qdb_test_close_fd(db->fd);
    db->fd = QDB__INVALID_FD;

    /* Push must fail. */
    int rc = qdb_push(db, "q", "should-not-land", 15);
    ASSERT_EQ(rc, QDB_ERR_IO);

    /* In-memory state must be completely unchanged. */
    ASSERT_EQ(db->state->msg_count,   saved_msg_count);
    ASSERT_EQ(db->state->queue_count, saved_queue_count);
    ASSERT_EQ(db->next_msg_id,        saved_next_id);
    ASSERT_EQ(db->log_end_offset,     saved_log_end);

    {
        struct queue_find_ctx fc;
        memset(&fc, 0, sizeof(fc));
        fc.name = "q";
        qdb__state_iter_queues(db->state, find_queue, &fc);
        ASSERT_EQ(fc.found.pending_count, saved_pending);
    }

    /* Mark the closed handle invalid so qdb_close only frees resources. */
    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_push_increments_log_end(void)
{
    const char *path = "push_logend.qdb";
    test_begin("each push advances log_end_offset in file header");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    uint64_t prev = db->log_end_offset;

    for (int i = 0; i < 5; i++) {
        uint64_t before = db->log_end_offset;
        ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
        /* log_end_offset must have grown. */
        ASSERT(db->log_end_offset > before);
    }

    /* Total growth = 5 records, each: 9-byte record hdr + 11-byte push payload
     * (msg_id(8)+name_len(1)+"q"(1)+data(1)) + 1-byte commit marker = 22 bytes. */
    uint64_t expected_growth = 5u * (QDB_REC_HDR_SIZE + QDB_PUSH_HDR_SIZE + 1u + 1u + 1u);
    ASSERT_EQ(db->log_end_offset - prev, expected_growth);

    /* Close and verify header on disk has the updated log_end. */
    uint64_t final_log_end = db->log_end_offset;
    qdb_close(db);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (db) {
        ASSERT_EQ(db->log_end_offset, final_log_end);
        qdb_close(db);
    }
    test_end();
    cleanup(path);
}

static void test_push_after_reopen_extends_log(void)
{
    const char *path = "push_extend.qdb";
    test_begin("push after reopen extends the existing log correctly");
    cleanup(path);

    /* Session 1: push 3 messages. */
    uint64_t end_after_s1;
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }
        ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "c", 1), QDB_OK);
        end_after_s1 = db->log_end_offset;
        qdb_close(db);
    }

    /* Session 2: push 2 more, then reopen and check all 5 are present. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        ASSERT_EQ(db->log_end_offset, end_after_s1);

        ASSERT_EQ(qdb_push(db, "q", "d", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "e", 1), QDB_OK);
        qdb_close(db);
    }

    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        ASSERT_EQ(db->state->msg_count, 5u);

        struct queue_find_ctx fc;
        memset(&fc, 0, sizeof(fc));
        fc.name = "q";
        qdb__state_iter_queues(db->state, find_queue, &fc);
        ASSERT_EQ(fc.found.pending_count, 5u);
        ASSERT_EQ(fc.found.pending_head,  1u);
        ASSERT_EQ(fc.found.pending_tail,  5u);

        qdb_close(db);
    }
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB push tests\n");
    printf("==============\n");

    test_push_one();
    test_push_many();
    test_push_multiple_queues();
    test_push_zero_length();
    test_push_invalid_queue_name();
    test_push_oversized_message();
    test_push_reopen_replay();
    test_push_msg_ids_never_reused();
    test_push_data_file_offset();
    test_push_append_failure_no_mutation();
    test_push_increments_log_end();
    test_push_after_reopen_extends_log();

    printf("==============\n");
    printf("Results: %d/%d passed",
           g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }
    printf("\n");
    return EXIT_SUCCESS;
}
