/*
 * test_stats.c — qdb_stats() and qdb_queue_stats() tests
 *
 * Exercises: NULL-argument validation, empty-database baseline, per-queue
 * and aggregate counts after push/pop/ack/nack, file-size growth, and
 * persistence across close/reopen.
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
 * Minimal test harness (identical to other test files)
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

static void cleanup(const char *path) { qdb_test_cleanup_files(path); }

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

static void test_stats_invalid_args(void)
{
    const char *path = "stats_inval.qdb";
    test_begin("invalid args: NULL db/out/queue return QDB_ERR_INVAL");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    qdb_stats_t      ds;
    qdb_queue_stats_t qs;

    /* qdb_stats NULL checks */
    ASSERT_EQ(qdb_stats(NULL, &ds), QDB_ERR_INVAL);
    ASSERT_EQ(qdb_stats(db,   NULL), QDB_ERR_INVAL);

    /* qdb_queue_stats NULL checks */
    ASSERT_EQ(qdb_queue_stats(NULL, "q", &qs), QDB_ERR_INVAL);
    ASSERT_EQ(qdb_queue_stats(db,   NULL, &qs), QDB_ERR_INVAL);
    ASSERT_EQ(qdb_queue_stats(db,   "q",  NULL), QDB_ERR_INVAL);

    /* Empty queue name */
    ASSERT_EQ(qdb_queue_stats(db, "", &qs), QDB_ERR_INVAL);

    /* Queue name over limit */
    {
        char long_name[QDB_QUEUE_NAME_MAX + 2];
        memset(long_name, 'x', QDB_QUEUE_NAME_MAX + 1);
        long_name[QDB_QUEUE_NAME_MAX + 1] = '\0';
        ASSERT_EQ(qdb_queue_stats(db, long_name, &qs), QDB_ERR_INVAL);
    }

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_empty_db(void)
{
    const char *path = "stats_empty.qdb";
    test_begin("empty database: all counts zero, file_size_bytes > 0");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    qdb_stats_t s;
    ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
    ASSERT_EQ(s.queue_count,   0u);
    ASSERT_EQ(s.pending_count, 0u);
    ASSERT_EQ(s.leased_count,  0u);
    ASSERT_EQ(s.acked_count,   0u);
    ASSERT(s.file_size_bytes >= QDB_HDR_SIZE); /* header is always present */

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_queue_noent(void)
{
    const char *path = "stats_noent.qdb";
    test_begin("qdb_queue_stats on nonexistent queue: QDB_ERR_NOENT");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    qdb_queue_stats_t qs;
    ASSERT_EQ(qdb_queue_stats(db, "no-such-queue", &qs), QDB_ERR_NOENT);

    /* After pushing to a different queue, the missing one is still absent. */
    ASSERT_EQ(qdb_push(db, "other", "x", 1), QDB_OK);
    ASSERT_EQ(qdb_queue_stats(db, "no-such-queue", &qs), QDB_ERR_NOENT);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_after_push(void)
{
    const char *path = "stats_push.qdb";
    test_begin("push: pending_count and queue_count correct");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "jobs",   "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs",   "b", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "alerts", "c", 1), QDB_OK);

    qdb_stats_t s;
    ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
    ASSERT_EQ(s.queue_count,   2u);
    ASSERT_EQ(s.pending_count, 3u);
    ASSERT_EQ(s.leased_count,  0u);
    ASSERT_EQ(s.acked_count,   0u);

    qdb_queue_stats_t qs;
    ASSERT_EQ(qdb_queue_stats(db, "jobs", &qs), QDB_OK);
    ASSERT_EQ(qs.pending_count, 2u);
    ASSERT_EQ(qs.leased_count,  0u);
    ASSERT_EQ(qs.acked_count,   0u);

    ASSERT_EQ(qdb_queue_stats(db, "alerts", &qs), QDB_OK);
    ASSERT_EQ(qs.pending_count, 1u);
    ASSERT_EQ(qs.leased_count,  0u);
    ASSERT_EQ(qs.acked_count,   0u);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_after_pop(void)
{
    const char *path = "stats_pop.qdb";
    test_begin("pop: pending decrements, leased increments");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    qdb_msg_free(&msg);

    qdb_stats_t s;
    ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
    ASSERT_EQ(s.pending_count, 1u);
    ASSERT_EQ(s.leased_count,  1u);
    ASSERT_EQ(s.acked_count,   0u);

    qdb_queue_stats_t qs;
    ASSERT_EQ(qdb_queue_stats(db, "q", &qs), QDB_OK);
    ASSERT_EQ(qs.pending_count, 1u);
    ASSERT_EQ(qs.leased_count,  1u);
    ASSERT_EQ(qs.acked_count,   0u);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_after_ack(void)
{
    const char *path = "stats_ack.qdb";
    test_begin("ack: leased decrements, acked increments");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    qdb_stats_t s;
    ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
    ASSERT_EQ(s.pending_count, 0u);
    ASSERT_EQ(s.leased_count,  0u);
    ASSERT_EQ(s.acked_count,   1u);

    qdb_queue_stats_t qs;
    ASSERT_EQ(qdb_queue_stats(db, "q", &qs), QDB_OK);
    ASSERT_EQ(qs.pending_count, 0u);
    ASSERT_EQ(qs.leased_count,  0u);
    ASSERT_EQ(qs.acked_count,   1u);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_after_nack(void)
{
    const char *path = "stats_nack.qdb";
    test_begin("nack: leased decrements, pending re-increments");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(qdb_nack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    qdb_stats_t s;
    ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
    ASSERT_EQ(s.pending_count, 1u);
    ASSERT_EQ(s.leased_count,  0u);
    ASSERT_EQ(s.acked_count,   0u);

    qdb_queue_stats_t qs;
    ASSERT_EQ(qdb_queue_stats(db, "q", &qs), QDB_OK);
    ASSERT_EQ(qs.pending_count, 1u);
    ASSERT_EQ(qs.leased_count,  0u);
    ASSERT_EQ(qs.acked_count,   0u);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_file_size_grows(void)
{
    const char *path = "stats_filesize.qdb";
    test_begin("file_size_bytes grows after each push");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    qdb_stats_t s0, s1, s2;
    ASSERT_EQ(qdb_stats(db, &s0), QDB_OK);
    ASSERT(s0.file_size_bytes > 0u);

    ASSERT_EQ(qdb_push(db, "q", "hello", 5), QDB_OK);
    ASSERT_EQ(qdb_stats(db, &s1), QDB_OK);
    ASSERT(s1.file_size_bytes > s0.file_size_bytes);

    ASSERT_EQ(qdb_push(db, "q", "world", 5), QDB_OK);
    ASSERT_EQ(qdb_stats(db, &s2), QDB_OK);
    ASSERT(s2.file_size_bytes > s1.file_size_bytes);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_multi_queue_aggregate(void)
{
    const char *path = "stats_multi.qdb";
    test_begin("multi-queue: aggregate matches sum of per-queue stats");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Push to 3 queues, pop+ack from one, pop (leased) from another. */
    ASSERT_EQ(qdb_push(db, "alpha", "a1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "alpha", "a2", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",  "b1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "gamma", "g1", 2), QDB_OK);

    /* Ack one message from alpha. */
    qdb_msg_t m1 = {0};
    ASSERT_EQ(qdb_pop(db, "alpha", &m1), QDB_OK);
    ASSERT_EQ(qdb_ack(db, m1.id, m1.lease_id), QDB_OK);
    qdb_msg_free(&m1);

    /* Leave beta message leased (no ack). */
    qdb_msg_t m2 = {0};
    ASSERT_EQ(qdb_pop(db, "beta", &m2), QDB_OK);
    qdb_msg_free(&m2);

    /* Expected state:
     *   alpha: pending=1, leased=0, acked=1
     *   beta:  pending=0, leased=1, acked=0
     *   gamma: pending=1, leased=0, acked=0
     *   total: pending=2, leased=1, acked=1
     */
    qdb_stats_t s;
    ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
    ASSERT_EQ(s.queue_count,   3u);
    ASSERT_EQ(s.pending_count, 2u);
    ASSERT_EQ(s.leased_count,  1u);
    ASSERT_EQ(s.acked_count,   1u);

    qdb_queue_stats_t qa, qb, qg;
    ASSERT_EQ(qdb_queue_stats(db, "alpha", &qa), QDB_OK);
    ASSERT_EQ(qdb_queue_stats(db, "beta",  &qb), QDB_OK);
    ASSERT_EQ(qdb_queue_stats(db, "gamma", &qg), QDB_OK);

    ASSERT_EQ(qa.pending_count + qb.pending_count + qg.pending_count, s.pending_count);
    ASSERT_EQ(qa.leased_count  + qb.leased_count  + qg.leased_count,  s.leased_count);
    ASSERT_EQ(qa.acked_count   + qb.acked_count   + qg.acked_count,   s.acked_count);

    ASSERT_EQ(qa.pending_count, 1u); ASSERT_EQ(qa.leased_count, 0u); ASSERT_EQ(qa.acked_count, 1u);
    ASSERT_EQ(qb.pending_count, 0u); ASSERT_EQ(qb.leased_count, 1u); ASSERT_EQ(qb.acked_count, 0u);
    ASSERT_EQ(qg.pending_count, 1u); ASSERT_EQ(qg.leased_count, 0u); ASSERT_EQ(qg.acked_count, 0u);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_stats_after_reopen(void)
{
    const char *path = "stats_reopen.qdb";
    test_begin("close/reopen: stats preserved across sessions");
    cleanup(path);

    /* Session 1: push 3, pop+ack 1. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "c", 1), QDB_OK);

        /* Ack the first message. */
        qdb_msg_t m = {0};
        ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
        ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
        qdb_msg_free(&m);

        qdb_close(db);
    }

    /* Session 2: replay reconstructs pending=2, acked=1. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        qdb_stats_t s;
        ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
        ASSERT_EQ(s.queue_count,   1u);
        ASSERT_EQ(s.pending_count, 2u);
        ASSERT_EQ(s.leased_count,  0u);
        ASSERT_EQ(s.acked_count,   1u);

        qdb_queue_stats_t qs;
        ASSERT_EQ(qdb_queue_stats(db, "q", &qs), QDB_OK);
        ASSERT_EQ(qs.pending_count, 2u);
        ASSERT_EQ(qs.leased_count,  0u);
        ASSERT_EQ(qs.acked_count,   1u);

        qdb_close(db);
    }

    test_end();
    cleanup(path);
}

static void test_stats_leased_across_reopen(void)
{
    const char *path = "stats_lease_reopen.qdb";
    test_begin("leased message survives reopen: leased_count non-zero");
    cleanup(path);

    /* Session 1: push 2, pop 1 (leaving it leased), close without acking. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        ASSERT_EQ(qdb_push(db, "q", "a", 1), QDB_OK);
        ASSERT_EQ(qdb_push(db, "q", "b", 1), QDB_OK);

        qdb_msg_t m = {0};
        ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
        /* Intentionally do NOT ack. */
        qdb_msg_free(&m);

        qdb_stats_t s;
        ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
        ASSERT_EQ(s.pending_count, 1u);
        ASSERT_EQ(s.leased_count,  1u);

        qdb_close(db); /* closes with active lease */
    }

    /* Session 2: replay reconstructs the LEASED state exactly. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        qdb_stats_t s;
        ASSERT_EQ(qdb_stats(db, &s), QDB_OK);
        ASSERT_EQ(s.queue_count,   1u);
        ASSERT_EQ(s.pending_count, 1u);
        ASSERT_EQ(s.leased_count,  1u); /* lease survived reopen */
        ASSERT_EQ(s.acked_count,   0u);

        qdb_queue_stats_t qs;
        ASSERT_EQ(qdb_queue_stats(db, "q", &qs), QDB_OK);
        ASSERT_EQ(qs.pending_count, 1u);
        ASSERT_EQ(qs.leased_count,  1u);
        ASSERT_EQ(qs.acked_count,   0u);

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
    printf("QDB stats tests\n");
    printf("===============\n");

    test_stats_invalid_args();
    test_stats_empty_db();
    test_stats_queue_noent();
    test_stats_after_push();
    test_stats_after_pop();
    test_stats_after_ack();
    test_stats_after_nack();
    test_stats_file_size_grows();
    test_stats_multi_queue_aggregate();
    test_stats_after_reopen();
    test_stats_leased_across_reopen();

    printf("===============\n");
    printf("Results: %d/%d passed",
           g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }
    printf("\n");
    return EXIT_SUCCESS;
}
