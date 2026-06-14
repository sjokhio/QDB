/*
 * test_compact_recommended.c — tests for qdb_compact_recommended()
 *
 * Covers all specified scenarios:
 *   1.  Empty database → 0
 *   2.  No acked messages → 0
 *   3.  Below threshold (acked < live) → 0
 *   4.  At threshold (acked == live) → 0
 *   5.  Above threshold (acked > live) → 1
 *   6.  All messages acked (no pending/leased) → 1
 *   7.  After qdb_compact() → 0
 *   8.  NULL db → QDB_ERR_INVAL
 *   9.  NULL out_recommended → QDB_ERR_INVAL
 *
 * Uses only the public API (qdb.h).
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
 * 1. Empty database → 0
 * ---------------------------------------------------------------------- */

static void test_empty_db(void)
{
    const char *path = "cr_empty.qdb";
    int         recommended = -1;

    test_begin("empty db: not recommended");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_compact_recommended(db, &recommended), QDB_OK);
    ASSERT_EQ(recommended, 0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 2. No acked messages → 0
 * ---------------------------------------------------------------------- */

static void test_no_acked(void)
{
    const char *path = "cr_no_acked.qdb";
    int         recommended = -1;

    test_begin("no acked messages: not recommended");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Push three messages, ack none. */
    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "b", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "c", 1), QDB_OK);

    ASSERT_EQ(qdb_compact_recommended(db, &recommended), QDB_OK);
    ASSERT_EQ(recommended, 0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 3. Below threshold: acked < live → 0
 * ---------------------------------------------------------------------- */

static void test_below_threshold(void)
{
    const char *path  = "cr_below.qdb";
    int         recommended = -1;
    qdb_msg_t   msg   = {0};

    test_begin("below threshold (acked < live): not recommended");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Push 4, ack 1, leave 3 pending. acked(1) < live(3). */
    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "b", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "c", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "d", 1), QDB_OK);

    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_compact_recommended(db, &recommended), QDB_OK);
    ASSERT_EQ(recommended, 0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 4. At threshold: acked == live → 0 (strictly greater required)
 * ---------------------------------------------------------------------- */

static void test_at_threshold(void)
{
    const char *path  = "cr_at.qdb";
    int         recommended = -1;
    qdb_msg_t   msg   = {0};

    test_begin("at threshold (acked == live): not recommended");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Push 4, ack 2, leave 2 pending. acked(2) == live(2). */
    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "b", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "c", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "d", 1), QDB_OK);

    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    msg = (qdb_msg_t){0};
    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_compact_recommended(db, &recommended), QDB_OK);
    ASSERT_EQ(recommended, 0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 5. Above threshold: acked > live → 1
 * ---------------------------------------------------------------------- */

static void test_above_threshold(void)
{
    const char *path  = "cr_above.qdb";
    int         recommended = -1;
    qdb_msg_t   msg   = {0};

    test_begin("above threshold (acked > live): recommended");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Push 4, ack 3, leave 1 pending. acked(3) > live(1). */
    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "b", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "c", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "d", 1), QDB_OK);

    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    msg = (qdb_msg_t){0};
    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    msg = (qdb_msg_t){0};
    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_compact_recommended(db, &recommended), QDB_OK);
    ASSERT_EQ(recommended, 1);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 6. All messages acked, nothing pending → 1
 * ---------------------------------------------------------------------- */

static void test_all_acked(void)
{
    const char *path  = "cr_all_acked.qdb";
    int         recommended = -1;
    qdb_msg_t   msg   = {0};

    test_begin("all messages acked (no pending/leased): recommended");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Push 3, ack all. acked(3) > live(0). */
    ASSERT_EQ(qdb_push(db, "jobs", "x", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "y", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "z", 1), QDB_OK);

    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    msg = (qdb_msg_t){0};
    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    msg = (qdb_msg_t){0};
    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_compact_recommended(db, &recommended), QDB_OK);
    ASSERT_EQ(recommended, 1);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 7. After qdb_compact() → 0 (acked_count resets to 0)
 * ---------------------------------------------------------------------- */

static void test_after_compact(void)
{
    const char *path  = "cr_after_compact.qdb";
    int         recommended = -1;
    qdb_msg_t   msg   = {0};

    test_begin("after qdb_compact(): not recommended");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Create a state that would recommend compaction. */
    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "b", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "c", 1), QDB_OK);

    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    msg = (qdb_msg_t){0};
    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    /* Verify compaction is recommended before compacting. */
    ASSERT_EQ(qdb_compact_recommended(db, &recommended), QDB_OK);
    ASSERT_EQ(recommended, 1);

    /* Compact; acked_count resets to 0. */
    ASSERT_EQ(qdb_compact(db), QDB_OK);

    /* Now compaction should not be recommended. */
    recommended = -1;
    ASSERT_EQ(qdb_compact_recommended(db, &recommended), QDB_OK);
    ASSERT_EQ(recommended, 0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 8. NULL db → QDB_ERR_INVAL
 * ---------------------------------------------------------------------- */

static void test_null_db(void)
{
    int recommended = -1;

    test_begin("NULL db: QDB_ERR_INVAL");

    ASSERT_EQ(qdb_compact_recommended(NULL, &recommended), QDB_ERR_INVAL);

    test_end();
}

/* -------------------------------------------------------------------------
 * 9. NULL out_recommended → QDB_ERR_INVAL
 * ---------------------------------------------------------------------- */

static void test_null_out(void)
{
    const char *path = "cr_null_out.qdb";

    test_begin("NULL out_recommended: QDB_ERR_INVAL");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_compact_recommended(db, NULL), QDB_ERR_INVAL);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB qdb_compact_recommended() tests\n");
    printf("====================================\n");

    test_empty_db();
    test_no_acked();
    test_below_threshold();
    test_at_threshold();
    test_above_threshold();
    test_all_acked();
    test_after_compact();
    test_null_db();
    test_null_out();

    printf("====================================\n");
    printf("Results: %d/%d passed",
           g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }
    printf("\n");
    return EXIT_SUCCESS;
}
