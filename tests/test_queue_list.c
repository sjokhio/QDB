/*
 * test_queue_list.c — tests for qdb_queue_list()
 *
 * Covers all specified scenarios:
 *   1.  Empty database → count = 0
 *   2.  Single queue   → count = 1, name correct
 *   3.  Three queues   → count = 3, all names present
 *   4.  Count-only mode (out=NULL, cap=0)
 *   5.  Buffer smaller than queue count → partial fill, correct total
 *   6.  Queue visible after all messages are acked
 *   7.  Queue visible after qdb_compact()
 *   8.  NULL db        → QDB_ERR_INVAL
 *   9.  NULL out_count → QDB_ERR_INVAL
 *  10.  NULL out with cap > 0 → QDB_ERR_INVAL
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
#define ASSERT_NOTNULL(p)  ASSERT((p) != NULL)

static void test_begin(const char *name)
{
    g_test_ok = 1;
    printf("  %-65s", name);
    fflush(stdout);
}
static void test_end(void) { printf("%s\n", g_test_ok ? "ok" : "FAILED"); }

static void cleanup(const char *path) { qdb_test_cleanup_files(path); }

/* Return 1 if @name appears in @names[0..count-1]. */
static int has_name(const qdb_queue_name_t *names, size_t count,
                    const char *name)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (strcmp(names[i].name, name) == 0) { return 1; }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * 1. Empty database
 * ---------------------------------------------------------------------- */

static void test_empty_db(void)
{
    const char      *path = "ql_empty.qdb";
    size_t           count = 99;
    qdb_queue_name_t names[4];

    test_begin("empty db: count = 0");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_queue_list(db, names, 4, &count), QDB_OK);
    ASSERT_EQ(count, (size_t)0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 2. Single queue
 * ---------------------------------------------------------------------- */

static void test_single_queue(void)
{
    const char      *path = "ql_single.qdb";
    size_t           count = 0;
    qdb_queue_name_t names[4];

    test_begin("single queue: count=1, name correct");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "jobs", "payload", 7), QDB_OK);

    ASSERT_EQ(qdb_queue_list(db, names, 4, &count), QDB_OK);
    ASSERT_EQ(count, (size_t)1);
    ASSERT_EQ(strcmp(names[0].name, "jobs"), 0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 3. Three queues — all names present (order unspecified)
 * ---------------------------------------------------------------------- */

static void test_three_queues(void)
{
    const char      *path = "ql_three.qdb";
    size_t           count = 0;
    qdb_queue_name_t names[8];

    test_begin("three queues: count=3, all names present");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "alpha",   "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",    "b", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "gamma",   "g", 1), QDB_OK);

    ASSERT_EQ(qdb_queue_list(db, names, 8, &count), QDB_OK);
    ASSERT_EQ(count, (size_t)3);
    ASSERT_EQ(has_name(names, count, "alpha"),  1);
    ASSERT_EQ(has_name(names, count, "beta"),   1);
    ASSERT_EQ(has_name(names, count, "gamma"),  1);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 4. Count-only mode: out=NULL, cap=0
 * ---------------------------------------------------------------------- */

static void test_count_only(void)
{
    const char *path = "ql_count_only.qdb";
    size_t      count = 0;

    test_begin("count-only (out=NULL, cap=0): correct count returned");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "q1", "x", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q2", "y", 1), QDB_OK);

    ASSERT_EQ(qdb_queue_list(db, NULL, 0, &count), QDB_OK);
    ASSERT_EQ(count, (size_t)2);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 5. Buffer smaller than queue count
 * ---------------------------------------------------------------------- */

static void test_small_buffer(void)
{
    const char      *path = "ql_small_buf.qdb";
    size_t           count = 0;
    qdb_queue_name_t names[2];  /* only room for 2, but 4 queues will exist */

    test_begin("small buffer: partial fill, correct total count");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "w", "1", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "x", "2", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "y", "3", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "z", "4", 1), QDB_OK);

    ASSERT_EQ(qdb_queue_list(db, names, 2, &count), QDB_OK);
    /* Total must be 4 even though only 2 were copied. */
    ASSERT_EQ(count, (size_t)4);
    /* The two entries that were written are valid NUL-terminated strings. */
    ASSERT_EQ(strlen(names[0].name) > 0, 1);
    ASSERT_EQ(strlen(names[1].name) > 0, 1);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 6. Queue visible after all messages acked
 * ---------------------------------------------------------------------- */

static void test_visible_after_ack(void)
{
    const char      *path = "ql_after_ack.qdb";
    size_t           count = 0;
    qdb_queue_name_t names[4];

    test_begin("queue visible after all messages acked");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "tasks", "work", 4), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "tasks", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    /* Queue entry persists after all messages are acked. */
    ASSERT_EQ(qdb_queue_list(db, names, 4, &count), QDB_OK);
    ASSERT_EQ(count, (size_t)1);
    ASSERT_EQ(strcmp(names[0].name, "tasks"), 0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 7. Queue visible after qdb_compact()
 * ---------------------------------------------------------------------- */

static void test_visible_after_compact(void)
{
    const char      *path = "ql_after_compact.qdb";
    size_t           count = 0;
    qdb_queue_name_t names[4];

    test_begin("queue visible after qdb_compact()");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "events", "e1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "events", "e2", 2), QDB_OK);

    /* Ack one so compaction has something to drop. */
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "events", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    /* Queue must still be present with the surviving message. */
    ASSERT_EQ(qdb_queue_list(db, names, 4, &count), QDB_OK);
    ASSERT_EQ(count, (size_t)1);
    ASSERT_EQ(strcmp(names[0].name, "events"), 0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 8. NULL db → QDB_ERR_INVAL
 * ---------------------------------------------------------------------- */

static void test_null_db(void)
{
    size_t           count = 0;
    qdb_queue_name_t names[4];

    test_begin("NULL db: QDB_ERR_INVAL");

    ASSERT_EQ(qdb_queue_list(NULL, names, 4, &count), QDB_ERR_INVAL);

    test_end();
}

/* -------------------------------------------------------------------------
 * 9. NULL out_count → QDB_ERR_INVAL
 * ---------------------------------------------------------------------- */

static void test_null_out_count(void)
{
    const char      *path = "ql_null_count.qdb";
    qdb_queue_name_t names[4];

    test_begin("NULL out_count: QDB_ERR_INVAL");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_queue_list(db, names, 4, NULL), QDB_ERR_INVAL);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 10. NULL out with cap > 0 → QDB_ERR_INVAL
 * ---------------------------------------------------------------------- */

static void test_null_out_nonzero_cap(void)
{
    const char *path = "ql_null_out.qdb";
    size_t      count = 0;

    test_begin("NULL out with cap>0: QDB_ERR_INVAL");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    ASSERT_EQ(qdb_queue_list(db, NULL, 4, &count), QDB_ERR_INVAL);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB qdb_queue_list() tests\n");
    printf("==========================\n");

    test_empty_db();
    test_single_queue();
    test_three_queues();
    test_count_only();
    test_small_buffer();
    test_visible_after_ack();
    test_visible_after_compact();
    test_null_db();
    test_null_out_count();
    test_null_out_nonzero_cap();

    printf("==========================\n");
    printf("Results: %d/%d passed",
           g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }
    printf("\n");
    return EXIT_SUCCESS;
}
