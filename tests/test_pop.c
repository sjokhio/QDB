/*
 * test_pop.c — qdb_pop() tests
 *
 * Exercises input validation, FIFO ordering, lease assignment, in-memory
 * state transitions, data integrity, close/reopen recovery, and the
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

static void test_pop_invalid_args(void)
{
    test_begin("invalid args: NULL db/queue/out_msg return QDB_ERR_INVAL");

    const char *path = "pop_test_inval.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t msg = {0};

    /* NULL db */
    ASSERT_EQ(qdb_pop(NULL, "q", &msg), QDB_ERR_INVAL);

    /* NULL queue */
    ASSERT_EQ(qdb_pop(db, NULL, &msg), QDB_ERR_INVAL);

    /* NULL out_msg */
    ASSERT_EQ(qdb_pop(db, "q", NULL), QDB_ERR_INVAL);

    /* empty queue name */
    ASSERT_EQ(qdb_pop(db, "", &msg), QDB_ERR_INVAL);

    /* queue name too long */
    char long_name[QDB_QUEUE_NAME_MAX + 2];
    memset(long_name, 'x', QDB_QUEUE_NAME_MAX + 1);
    long_name[QDB_QUEUE_NAME_MAX + 1] = '\0';
    ASSERT_EQ(qdb_pop(db, long_name, &msg), QDB_ERR_INVAL);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: pop from empty queue
 * ---------------------------------------------------------------------- */

static void test_pop_empty_queue(void)
{
    test_begin("pop from empty queue returns QDB_ERR_EMPTY");

    const char *path = "pop_test_empty.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "nothing", &msg), QDB_ERR_EMPTY);

    /* After a push, popping a different queue name is also empty. */
    ASSERT_EQ(qdb_push(db, "jobs", "hello", 5), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "other", &msg), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: pop one message
 * ---------------------------------------------------------------------- */

static void test_pop_one_message(void)
{
    test_begin("pop one message: id, queue name, data, len correct");

    const char *path = "pop_test_one.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    const char *payload = "hello world";
    ASSERT_EQ(qdb_push(db, "jobs", payload, 11), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);

    ASSERT_EQ(msg.id, (uint64_t)1);
    ASSERT_NOTNULL(msg.queue);
    ASSERT_EQ(strcmp(msg.queue, "jobs"), 0);
    ASSERT_NOTNULL(msg.data);
    ASSERT_EQ(msg.len, (size_t)11);
    ASSERT_EQ(memcmp(msg.data, payload, 11), 0);

    qdb_msg_free(&msg);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: pop assigns a non-zero lease_id
 * ---------------------------------------------------------------------- */

static void test_pop_assigns_lease_id(void)
{
    test_begin("pop assigns non-zero lease_id");

    const char *path = "pop_test_lease.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_NE(msg.lease_id, (uint64_t)0);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: FIFO order preserved across pop
 * ---------------------------------------------------------------------- */

static void test_pop_fifo_order(void)
{
    test_begin("pop 5 messages: FIFO order preserved");

    const char *path = "pop_test_fifo.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    for (int i = 0; i < 5; i++) {
        char buf[4];
        buf[0] = (char)i;
        buf[1] = buf[2] = buf[3] = 0;
        ASSERT_EQ(qdb_push(db, "fifo", buf, 1), QDB_OK);
    }

    for (int i = 0; i < 5; i++) {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, "fifo", &msg), QDB_OK);
        ASSERT_EQ(msg.id, (uint64_t)(i + 1));
        ASSERT_EQ(msg.len, (size_t)1);
        ASSERT_EQ(((const char *)msg.data)[0], (char)i);
        qdb_msg_free(&msg);
    }

    /* Queue exhausted */
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "fifo", &msg), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: lease_id monotonically increasing across pops
 * ---------------------------------------------------------------------- */

static void test_pop_lease_id_monotonic(void)
{
    test_begin("lease_id increases monotonically across pops");

    const char *path = "pop_test_mono.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(qdb_push(db, "q", "d", 1), QDB_OK);
    }

    uint64_t prev_lease = 0;
    for (int i = 0; i < 5; i++) {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        ASSERT(msg.lease_id > prev_lease);
        prev_lease = msg.lease_id;
        qdb_msg_free(&msg);
    }

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: in-memory state transitions after pop
 * ---------------------------------------------------------------------- */

static void test_pop_state_transitions(void)
{
    test_begin("pop transitions message PENDING→LEASED, updates queue counts");

    const char *path = "pop_test_state.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)2);
    ASSERT_EQ(q->leased_count,  (uint32_t)0);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    ASSERT_EQ(q->pending_count, (uint32_t)1);
    ASSERT_EQ(q->leased_count,  (uint32_t)1);

    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_LEASED);
    ASSERT_EQ(m->lease_id, msg.lease_id);
    ASSERT_NE(m->lease_expiry_us, (uint64_t)0);

    /* Lease entry in the lease table */
    struct qdb__lease *l = qdb__lease_get(db->state, msg.lease_id);
    ASSERT_NOTNULL(l);
    ASSERT_EQ(l->msg_id, msg.id);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: zero-length message
 * ---------------------------------------------------------------------- */

static void test_pop_zero_length(void)
{
    test_begin("pop zero-length message: data is NULL, len is 0");

    const char *path = "pop_test_zero.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", NULL, 0), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    ASSERT_EQ(msg.len, (size_t)0);
    ASSERT_NULL(msg.data);
    ASSERT_NOTNULL(msg.queue);

    qdb_msg_free(&msg);
    ASSERT_NULL(msg.data);
    ASSERT_NULL(msg.queue);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: data integrity — exact bytes round-trip
 * ---------------------------------------------------------------------- */

static void test_pop_data_integrity(void)
{
    test_begin("pop data integrity: all 256 byte values survive round-trip");

    const char *path = "pop_test_integrity.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    uint8_t original[256];
    for (int i = 0; i < 256; i++) {
        original[i] = (uint8_t)i;
    }

    ASSERT_EQ(qdb_push(db, "q", original, 256), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(msg.len, (size_t)256);
    ASSERT_NOTNULL(msg.data);
    ASSERT_EQ(memcmp(msg.data, original, 256), 0);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: multiple queues independent
 * ---------------------------------------------------------------------- */

static void test_pop_multiple_queues(void)
{
    test_begin("pop from multiple queues: each queue has independent FIFO");

    const char *path = "pop_test_multi.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "alpha", "a1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",  "b1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "alpha", "a2", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",  "b2", 2), QDB_OK);

    qdb_msg_t m = {0};

    ASSERT_EQ(qdb_pop(db, "alpha", &m), QDB_OK);
    ASSERT_EQ(memcmp(m.data, "a1", 2), 0);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "beta", &m), QDB_OK);
    ASSERT_EQ(memcmp(m.data, "b1", 2), 0);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "alpha", &m), QDB_OK);
    ASSERT_EQ(memcmp(m.data, "a2", 2), 0);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "beta", &m), QDB_OK);
    ASSERT_EQ(memcmp(m.data, "b2", 2), 0);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "alpha", &m), QDB_ERR_EMPTY);
    ASSERT_EQ(qdb_pop(db, "beta",  &m), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: popped message is LEASED after close/reopen
 * ---------------------------------------------------------------------- */

static void test_pop_reopen_leased_state(void)
{
    test_begin("pop then reopen: message is LEASED, remaining messages PENDING");

    const char *path = "pop_test_reopen.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "first",  5), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "second", 6), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    uint64_t leased_id  = msg.id;
    uint64_t lease_id   = msg.lease_id;
    qdb_msg_free(&msg);

    qdb_close(db);

    /* Reopen */
    db = qdb_open(path);
    ASSERT_NOTNULL(db);

    /* The popped message must be LEASED in the replayed state */
    struct qdb__msg *m = qdb__msg_get(db->state, leased_id);
    ASSERT_NOTNULL(m);
    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_LEASED);
    ASSERT_EQ(m->lease_id, lease_id);

    /* The second message must still be PENDING */
    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    ASSERT_EQ(q->pending_count, (uint32_t)1);
    ASSERT_EQ(q->leased_count,  (uint32_t)1);

    /* Pop the second message */
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(msg.len, (size_t)6);
    ASSERT_EQ(memcmp(msg.data, "second", 6), 0);
    qdb_msg_free(&msg);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: lease_id persists across reopen
 * ---------------------------------------------------------------------- */

static void test_pop_lease_id_persists(void)
{
    test_begin("lease_id survives close/reopen; next_lease_id never regresses");

    const char *path = "pop_test_lsid.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);

    qdb_msg_t m1 = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m1), QDB_OK);
    uint64_t first_lease = m1.lease_id;
    qdb_msg_free(&m1);

    qdb_close(db);
    db = qdb_open(path);
    ASSERT_NOTNULL(db);

    /* next pop must get a lease_id strictly greater than the first */
    qdb_msg_t m2 = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m2), QDB_OK);
    ASSERT(m2.lease_id > first_lease);
    qdb_msg_free(&m2);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: qdb_msg_free zeroes the struct
 * ---------------------------------------------------------------------- */

static void test_pop_msg_free_zeroes(void)
{
    test_begin("qdb_msg_free zeroes all fields; NULL is a safe no-op");

    const char *path = "pop_test_free.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "data", 4), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    ASSERT_NE(msg.id, (uint64_t)0);
    ASSERT_NOTNULL(msg.queue);
    ASSERT_NOTNULL(msg.data);

    qdb_msg_free(&msg);

    ASSERT_EQ(msg.id,       (uint64_t)0);
    ASSERT_EQ(msg.lease_id, (uint64_t)0);
    ASSERT_NULL(msg.queue);
    ASSERT_NULL(msg.data);
    ASSERT_EQ(msg.len, (size_t)0);

    /* Second free is a safe no-op */
    qdb_msg_free(&msg);
    /* NULL pointer is safe */
    qdb_msg_free(NULL);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: heap-allocated buffers survive subsequent API calls
 * ---------------------------------------------------------------------- */

static void test_pop_data_outlives_next_call(void)
{
    test_begin("popped data buffer outlives subsequent push/pop calls");

    const char *path = "pop_test_outlive.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "first",  5), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "second", 6), QDB_OK);

    qdb_msg_t m1 = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m1), QDB_OK);

    /* Pop a second message — must not invalidate m1's buffers */
    qdb_msg_t m2 = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m2), QDB_OK);

    /* m1's data must still be intact */
    ASSERT_EQ(m1.len, (size_t)5);
    ASSERT_EQ(memcmp(m1.data, "first", 5), 0);
    ASSERT_EQ(strcmp(m1.queue, "q"), 0);

    qdb_msg_free(&m1);
    qdb_msg_free(&m2);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: I/O failure on lease write — no state change
 * ---------------------------------------------------------------------- */

#if !defined(_WIN32)
static void test_pop_io_failure_no_state_change(void)
{
    test_begin("I/O failure on lease write: in-memory state unchanged");

    const char *path = "pop_test_iofail.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "payload", 7), QDB_OK);

    uint64_t end_before    = db->log_end_offset;
    uint64_t lease_before  = db->next_lease_id;

    struct qdb__queue *q = qdb__queue_get(db->state, "q", 1);
    ASSERT_NOTNULL(q);
    uint32_t pending_before = q->pending_count;
    uint32_t leased_before  = q->leased_count;

    /* Destroy the fd so the lease write will fail */
    int saved_fd = (int)db->fd;
    close(saved_fd);

    qdb_msg_t msg = {0};
    int rc = qdb_pop(db, "q", &msg);
    ASSERT_NE(rc, QDB_OK);

    /* No state must have changed */
    ASSERT_EQ(db->log_end_offset, end_before);
    ASSERT_EQ(db->next_lease_id,  lease_before);
    ASSERT_EQ(q->pending_count,   pending_before);
    ASSERT_EQ(q->leased_count,    leased_before);
    ASSERT_NULL(msg.queue);
    ASSERT_NULL(msg.data);

    /* Prevent qdb_close from double-closing the fd (it's already closed) */
    db->fd = QDB__INVALID_FD;
    qdb_close(db);
    cleanup(path);
    test_end();
}
#endif /* !_WIN32 */

/* -------------------------------------------------------------------------
 * Test: log_end_offset advances by correct amount after each pop
 * ---------------------------------------------------------------------- */

static void test_pop_log_end_advances(void)
{
    test_begin("each pop advances log_end_offset by one lease record size");

    const char *path = "pop_test_logend.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);

    /* RT_MSG_LEASE record size:
     *   header (9) + payload (24) + commit marker (1) = 34 bytes */
    uint64_t expected_step = QDB_REC_HDR_SIZE + QDB_PAYLOAD_LEASE_SIZE + 1u;

    uint64_t before = db->log_end_offset;

    qdb_msg_t m = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(db->log_end_offset, before + expected_step);

    before = db->log_end_offset;
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    qdb_msg_free(&m);
    ASSERT_EQ(db->log_end_offset, before + expected_step);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test: push / pop / push / pop interleaved
 * ---------------------------------------------------------------------- */

static void test_pop_interleaved(void)
{
    test_begin("push/pop/push/pop interleaved: log and state remain consistent");

    const char *path = "pop_test_interleave.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    for (int round = 0; round < 3; round++) {
        ASSERT_EQ(qdb_push(db, "q", "ping", 4), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "pong", 4), QDB_OK);

        qdb_msg_t m = {0};
        ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
        ASSERT_EQ(memcmp(m.data, "ping", 4), 0);
        qdb_msg_free(&m);

        ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
        ASSERT_EQ(memcmp(m.data, "pong", 4), 0);
        qdb_msg_free(&m);

        ASSERT_EQ(qdb_pop(db, "q", &m), QDB_ERR_EMPTY);
    }

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB pop tests\n");
    printf("=============\n");

    test_pop_invalid_args();
    test_pop_empty_queue();
    test_pop_one_message();
    test_pop_assigns_lease_id();
    test_pop_fifo_order();
    test_pop_lease_id_monotonic();
    test_pop_state_transitions();
    test_pop_zero_length();
    test_pop_data_integrity();
    test_pop_multiple_queues();
    test_pop_reopen_leased_state();
    test_pop_lease_id_persists();
    test_pop_msg_free_zeroes();
    test_pop_data_outlives_next_call();
#if !defined(_WIN32)
    test_pop_io_failure_no_state_change();
#endif
    test_pop_log_end_advances();
    test_pop_interleaved();

    printf("=============\n");
    printf("Results: %d/%d passed", g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }

    printf("\n");
    return EXIT_SUCCESS;
}
