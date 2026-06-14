/*
 * test_compact.c — qdb_compact() tests
 *
 * Verifies that compaction:
 *   - excludes ACKED messages from the compact file
 *   - preserves PENDING messages with FIFO order
 *   - preserves LEASED messages with original lease_id and expiry
 *   - keeps next_msg_id and next_lease_id monotonic
 *   - produces a smaller file when many messages have been acked
 *   - leaves the database fully usable after compact
 *   - survives a crash/reopen cycle after compact
 *   - cleans up a stale -compact sidecar during startup
 *
 * SPDX-License-Identifier: MIT
 */

#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#elif !defined(_CRT_SECURE_NO_WARNINGS)
#  define _CRT_SECURE_NO_WARNINGS
#endif

#include "qdb.h"

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

#define ASSERT(expr)                                                         \
    do {                                                                     \
        g_tests_run++;                                                       \
        if (!(expr)) {                                                       \
            fprintf(stderr, "  FAIL  %s:%d  %s\n",                         \
                    __FILE__, __LINE__, #expr);                              \
            g_tests_failed++;                                                \
            g_test_ok = 0;                                                   \
        }                                                                    \
    } while (0)

#define ASSERT_EQ(a, b)    ASSERT((a) == (b))
#define ASSERT_NULL(p)     ASSERT((p) == NULL)
#define ASSERT_NOTNULL(p)  ASSERT((p) != NULL)

static void test_begin(const char *name)
{
    g_test_ok = 1;
    printf("  %-62s", name);
    fflush(stdout);
}

static void test_end(void)
{
    printf("%s\n", g_test_ok ? "ok" : "FAILED");
}

static void cleanup(const char *path)
{
    qdb_test_cleanup_files(path);
}

/* -------------------------------------------------------------------------
 * Test 1: compact on an empty database
 * ---------------------------------------------------------------------- */

static void test_compact_empty(void)
{
    const char *path = "cpt_empty.qdb";
    qdb_t      *db;
    qdb_stats_t stats;

    test_begin("compact empty database");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    ASSERT_EQ(qdb_stats(db, &stats), QDB_OK);
    ASSERT_EQ(stats.pending_count, 0u);
    ASSERT_EQ(stats.leased_count,  0u);
    ASSERT_EQ(stats.acked_count,   0u);

    /* Database must still be usable after compact. */
    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    ASSERT_EQ(qdb_stats(db, &stats), QDB_OK);
    ASSERT_EQ(stats.pending_count, 1u);

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 2: compact preserves all pending messages
 * ---------------------------------------------------------------------- */

static void test_compact_preserves_pending(void)
{
    const char *path = "cpt_pending.qdb";
    qdb_t      *db;
    qdb_stats_t stats;
    unsigned int i;

    test_begin("compact preserves all pending messages");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    for (i = 0; i < 5u; i++) {
        ASSERT_EQ(qdb_push(db, "q", "msg", 3), QDB_OK);
    }

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    ASSERT_EQ(qdb_stats(db, &stats), QDB_OK);
    ASSERT_EQ(stats.pending_count, 5u);
    ASSERT_EQ(stats.leased_count,  0u);
    ASSERT_EQ(stats.acked_count,   0u);

    /* All 5 messages must be poppable and ackable. */
    for (i = 0; i < 5u; i++) {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
        qdb_msg_free(&msg);
    }
    {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_ERR_EMPTY);
    }

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 3: compact excludes acked messages; mixed state
 * ---------------------------------------------------------------------- */

static void test_compact_excludes_acked(void)
{
    const char *path = "cpt_mixed.qdb";
    qdb_t      *db;
    qdb_stats_t stats;
    qdb_msg_t   leased = {0};

    test_begin("compact excludes acked, preserves pending+leased");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    /* Push 5, ack 2, pop (hold) 1 — leaves 2 pending, 1 leased, 2 acked. */
    {
        unsigned int i;
        for (i = 0; i < 5u; i++) {
            ASSERT_EQ(qdb_push(db, "q", "msg", 3), QDB_OK);
        }
        for (i = 0; i < 2u; i++) {
            qdb_msg_t msg = {0};
            ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
            ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
            qdb_msg_free(&msg);
        }
    }
    ASSERT_EQ(qdb_pop(db, "q", &leased), QDB_OK);  /* holds the lease */

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    ASSERT_EQ(qdb_stats(db, &stats), QDB_OK);
    ASSERT_EQ(stats.pending_count, 2u);
    ASSERT_EQ(stats.leased_count,  1u);
    ASSERT_EQ(stats.acked_count,   0u);   /* acked records gone from compact file */

    /* Resolve the held lease. */
    ASSERT_EQ(qdb_ack(db, leased.id, leased.lease_id), QDB_OK);
    qdb_msg_free(&leased);

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 4: compact preserves FIFO order
 * ---------------------------------------------------------------------- */

static void test_compact_preserves_fifo(void)
{
    const char *path = "cpt_fifo.qdb";
    qdb_t      *db;
    qdb_msg_t   msg  = {0};

    test_begin("compact preserves FIFO queue order");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    ASSERT_EQ(qdb_push(db, "q", "first",  5), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "second", 6), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "third",  5), QDB_OK);

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT(msg.len == 5 && memcmp(msg.data, "first", 5) == 0);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT(msg.len == 6 && memcmp(msg.data, "second", 6) == 0);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT(msg.len == 5 && memcmp(msg.data, "third", 5) == 0);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_ERR_EMPTY);

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 5: compact preserves active leases (lease_id survives)
 * ---------------------------------------------------------------------- */

static void test_compact_preserves_leases(void)
{
    const char *path = "cpt_lease.qdb";
    qdb_t      *db;
    qdb_msg_t   msg  = {0};
    uint64_t    saved_msg_id;
    uint64_t    saved_lease_id;
    qdb_stats_t stats;

    test_begin("compact preserves active lease_id and state");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    ASSERT_EQ(qdb_push(db, "q", "payload", 7), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    saved_msg_id   = msg.id;
    saved_lease_id = msg.lease_id;
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    /* Message should still be LEASED — not poppable. */
    ASSERT_EQ(qdb_stats(db, &stats), QDB_OK);
    ASSERT_EQ(stats.pending_count, 0u);
    ASSERT_EQ(stats.leased_count,  1u);

    /* Original lease_id must still work. */
    ASSERT_EQ(qdb_ack(db, saved_msg_id, saved_lease_id), QDB_OK);

    ASSERT_EQ(qdb_stats(db, &stats), QDB_OK);
    ASSERT_EQ(stats.pending_count, 0u);
    ASSERT_EQ(stats.leased_count,  0u);

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 6: next_msg_id remains monotonic after compact
 * ---------------------------------------------------------------------- */

static void test_compact_next_msg_id_monotonic(void)
{
    const char *path = "cpt_msgid.qdb";
    qdb_t      *db;
    uint64_t    last_id = 0;
    unsigned int i;

    test_begin("next_msg_id monotonic after compact");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    /* Push 5 messages and ack all of them; record the last id. */
    for (i = 0; i < 5u; i++) {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        last_id = msg.id;
        ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
        qdb_msg_free(&msg);
    }

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    /* Push after compact: new ID must be greater than the last pre-compact ID. */
    {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_push(db, "q", "y", 1), QDB_OK);
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        ASSERT(msg.id > last_id);
        ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
        qdb_msg_free(&msg);
    }

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 7: next_lease_id remains monotonic after compact
 * ---------------------------------------------------------------------- */

static void test_compact_next_lease_id_monotonic(void)
{
    const char *path = "cpt_leaseid.qdb";
    qdb_t      *db;
    uint64_t    last_lease_id = 0;
    unsigned int i;

    test_begin("next_lease_id monotonic after compact");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    /* Push 3, pop+ack all 3; record the last lease_id used. */
    for (i = 0; i < 3u; i++) {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        last_lease_id = msg.lease_id;
        ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
        qdb_msg_free(&msg);
    }

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    /* Next pop after compact must use a lease_id greater than any previous. */
    {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_push(db, "q", "y", 1), QDB_OK);
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        ASSERT(msg.lease_id > last_lease_id);
        ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
        qdb_msg_free(&msg);
    }

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 8: compact + close + reopen recovers correctly
 * ---------------------------------------------------------------------- */

static void test_compact_then_reopen(void)
{
    const char *path = "cpt_reopen.qdb";
    qdb_t      *db;
    qdb_stats_t stats;

    test_begin("compact followed by close and reopen recovers correctly");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    /* Push 5 to "a", 3 to "b", ack 3 from "a" — leaves 2 pending in "a". */
    {
        unsigned int i;
        for (i = 0; i < 5u; i++) {
            ASSERT_EQ(qdb_push(db, "a", "msg", 3), QDB_OK);
        }
        for (i = 0; i < 3u; i++) {
            qdb_msg_t msg = {0};
            ASSERT_EQ(qdb_pop(db, "a", &msg), QDB_OK);
            ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
            qdb_msg_free(&msg);
        }
        for (i = 0; i < 3u; i++) {
            ASSERT_EQ(qdb_push(db, "b", "msg", 3), QDB_OK);
        }
    }

    ASSERT_EQ(qdb_compact(db), QDB_OK);
    qdb_close(db);

    /* Reopen and verify state is consistent with what was compacted. */
    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    {
        qdb_queue_stats_t qs;
        ASSERT_EQ(qdb_queue_stats(db, "a", &qs), QDB_OK);
        ASSERT_EQ(qs.pending_count, 2u);
        ASSERT_EQ(qs.leased_count,  0u);
        ASSERT_EQ(qs.acked_count,   0u);   /* acked records absent in compact file */

        ASSERT_EQ(qdb_queue_stats(db, "b", &qs), QDB_OK);
        ASSERT_EQ(qs.pending_count, 3u);
        ASSERT_EQ(qs.leased_count,  0u);
    }

    ASSERT_EQ(qdb_stats(db, &stats), QDB_OK);
    ASSERT_EQ(stats.pending_count, 5u);

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 9: file size shrinks after compacting many acked messages
 * ---------------------------------------------------------------------- */

static void test_compact_file_shrinks(void)
{
    const char  *path = "cpt_shrink.qdb";
    qdb_t       *db;
    qdb_stats_t  stats_before;
    qdb_stats_t  stats_after;
    /* 128-byte payload makes each PUSH record noticeably larger. */
    char payload[128];
    unsigned int i;

    test_begin("file size shrinks after compacting acked messages");
    cleanup(path);
    memset(payload, 'X', sizeof(payload));

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    /* Push and ack 10 messages with a 128-byte payload each. */
    for (i = 0; i < 10u; i++) {
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_push(db, "q", payload, sizeof(payload)), QDB_OK);
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
        qdb_msg_free(&msg);
    }
    /* Leave 1 small pending message so compact has something to preserve. */
    ASSERT_EQ(qdb_push(db, "q", "z", 1), QDB_OK);

    ASSERT_EQ(qdb_stats(db, &stats_before), QDB_OK);

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    ASSERT_EQ(qdb_stats(db, &stats_after), QDB_OK);
    ASSERT(stats_after.file_size_bytes < stats_before.file_size_bytes);
    ASSERT_EQ(stats_after.pending_count, 1u);
    ASSERT_EQ(stats_after.acked_count,   0u);

    qdb_close(db);

done:
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 10: stale -compact sidecar is cleaned up during qdb_open()
 * ---------------------------------------------------------------------- */

static void test_compact_stale_sidecar_cleanup(void)
{
    const char *path         = "cpt_stale.qdb";
    const char *compact_path = "cpt_stale.qdb-compact";
    qdb_t      *db;
    FILE       *f;
    qdb_stats_t stats;

    test_begin("stale -compact sidecar cleaned up on open");
    cleanup(path);
    (void)qdb_test_remove_file(compact_path);

    /* Create a real database with data. */
    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }
    ASSERT_EQ(qdb_push(db, "q", "hello", 5), QDB_OK);
    qdb_close(db);

    /* Simulate an interrupted compact: leave a stale -compact sidecar. */
    f = fopen(compact_path, "wb");
    ASSERT_NOTNULL(f);
    if (f) {
        fputc(0, f);
        fclose(f);
    }

    /* Reopen: the stale sidecar must not prevent opening or corrupt state. */
    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { goto done; }

    ASSERT_EQ(qdb_stats(db, &stats), QDB_OK);
    ASSERT_EQ(stats.pending_count, 1u);

    /* Verify the stale compact file was deleted. */
    {
        FILE *check = fopen(compact_path, "rb");
        ASSERT_NULL(check);
        if (check) { fclose(check); }
    }

    qdb_close(db);

done:
    cleanup(path);
    (void)qdb_test_remove_file(compact_path);
    test_end();
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    puts("QDB compact tests");

    test_compact_empty();
    test_compact_preserves_pending();
    test_compact_excludes_acked();
    test_compact_preserves_fifo();
    test_compact_preserves_leases();
    test_compact_next_msg_id_monotonic();
    test_compact_next_lease_id_monotonic();
    test_compact_then_reopen();
    test_compact_file_shrinks();
    test_compact_stale_sidecar_cleanup();

    printf("\n%d tests, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
