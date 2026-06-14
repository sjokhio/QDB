/*
 * test_boundaries.c — API boundary tests for QDB
 *
 * Verifies documented limits that cross multiple operations:
 *   - Empty payload (data=NULL, len=0): push → pop → ack round-trip
 *   - Maximum queue name (255 bytes): push → pop → ack → queue_stats
 *   - Over-limit queue name (256 bytes): rejected at push/pop/ack
 *   - compact preserves empty-payload messages
 *   - compact works with a 255-byte queue name
 *
 * Uses only the public API (qdb.h); no internal headers.
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
    printf("  %-65s", name);
    fflush(stdout);
}
static void test_end(void) { printf("%s\n", g_test_ok ? "ok" : "FAILED"); }

static void cleanup(const char *path) { qdb_test_cleanup_files(path); }

/* Build a queue name of exactly `len` 'q' bytes, NUL-terminated. */
static void make_name(char *buf, size_t buf_sz, size_t len)
{
    size_t i;
    if (len >= buf_sz) { len = buf_sz - 1; }
    for (i = 0; i < len; i++) { buf[i] = 'q'; }
    buf[len] = '\0';
}

/* -------------------------------------------------------------------------
 * 1. Empty payload — push/pop/ack round-trip
 * ---------------------------------------------------------------------- */

static void test_empty_payload_roundtrip(void)
{
    const char *path = "bnd_empty_payload.qdb";
    test_begin("empty payload (NULL,0): push → pop → ack round-trip");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "q", NULL, 0), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(msg.len, (size_t)0);
    ASSERT_NULL(msg.data);
    ASSERT_NOTNULL(msg.queue);

    uint64_t mid = msg.id;
    uint64_t lid = msg.lease_id;
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_ack(db, mid, lid), QDB_OK);

    /* Queue is now empty. */
    qdb_msg_t msg2 = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg2), QDB_ERR_EMPTY);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_empty_payload_reopen(void)
{
    const char *path = "bnd_empty_reopen.qdb";
    test_begin("empty payload: survives close/reopen");
    cleanup(path);

    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }
        ASSERT_EQ(qdb_push(db, "q", NULL, 0), QDB_OK);
        qdb_close(db);
    }

    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        ASSERT_EQ(msg.len, (size_t)0);
        ASSERT_NULL(msg.data);
        ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
        qdb_msg_free(&msg);

        qdb_close(db);
    }

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 2. Maximum queue name (255 bytes) — push/pop/ack/stats
 * ---------------------------------------------------------------------- */

static void test_max_name_roundtrip(void)
{
    char        name[QDB_QUEUE_NAME_MAX + 1];
    const char *path = "bnd_maxname.qdb";

    test_begin("255-byte queue name: push → pop → ack round-trip");
    make_name(name, sizeof(name), QDB_QUEUE_NAME_MAX);
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, name, "hello", 5), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, name, &msg), QDB_OK);
    ASSERT_EQ(msg.len, (size_t)5);
    ASSERT_NOTNULL(msg.data);
    if (msg.data) { ASSERT_EQ(memcmp(msg.data, "hello", 5), 0); }
    ASSERT_NOTNULL(msg.queue);
    if (msg.queue) { ASSERT_EQ(strcmp(msg.queue, name), 0); }

    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_max_name_queue_stats(void)
{
    char        name[QDB_QUEUE_NAME_MAX + 1];
    const char *path = "bnd_maxname_stats.qdb";

    test_begin("255-byte queue name: qdb_queue_stats works");
    make_name(name, sizeof(name), QDB_QUEUE_NAME_MAX);
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, name, "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, name, "b", 1), QDB_OK);

    qdb_queue_stats_t qs = {0};
    ASSERT_EQ(qdb_queue_stats(db, name, &qs), QDB_OK);
    ASSERT_EQ(qs.pending_count, (uint64_t)2);
    ASSERT_EQ(qs.leased_count,  (uint64_t)0);

    /* Pop one message and check leased_count. */
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, name, &msg), QDB_OK);

    ASSERT_EQ(qdb_queue_stats(db, name, &qs), QDB_OK);
    ASSERT_EQ(qs.pending_count, (uint64_t)1);
    ASSERT_EQ(qs.leased_count,  (uint64_t)1);

    qdb_ack(db, msg.id, msg.lease_id);
    qdb_msg_free(&msg);

    qdb_close(db);
    test_end();
    cleanup(path);
}

static void test_max_name_reopen(void)
{
    char        name[QDB_QUEUE_NAME_MAX + 1];
    const char *path = "bnd_maxname_reopen.qdb";

    test_begin("255-byte queue name: survives close/reopen");
    make_name(name, sizeof(name), QDB_QUEUE_NAME_MAX);
    cleanup(path);

    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }
        ASSERT_EQ(qdb_push(db, name, "x", 1), QDB_OK);
        qdb_close(db);
    }

    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        qdb_queue_stats_t qs = {0};
        ASSERT_EQ(qdb_queue_stats(db, name, &qs), QDB_OK);
        ASSERT_EQ(qs.pending_count, (uint64_t)1);

        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, name, &msg), QDB_OK);
        ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
        qdb_msg_free(&msg);

        qdb_close(db);
    }

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 3. Over-limit queue name (256 bytes) — rejected at push, pop, ack
 * ---------------------------------------------------------------------- */

static void test_overlimit_name_rejected(void)
{
    /* Name is one byte over QDB_QUEUE_NAME_MAX. */
    char        name[QDB_QUEUE_NAME_MAX + 2];
    const char *path = "bnd_overlimit.qdb";

    test_begin("256-byte queue name: QDB_ERR_INVAL at push/pop/stats");
    make_name(name, sizeof(name), QDB_QUEUE_NAME_MAX + 1);
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, name, "x", 1), QDB_ERR_INVAL);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, name, &msg), QDB_ERR_INVAL);

    qdb_queue_stats_t qs = {0};
    ASSERT_EQ(qdb_queue_stats(db, name, &qs), QDB_ERR_INVAL);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 4. compact preserves empty-payload messages
 * ---------------------------------------------------------------------- */

static void test_compact_empty_payload(void)
{
    const char *path = "bnd_cpt_empty.qdb";
    test_begin("compact preserves empty-payload messages");
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Push two empty messages and one normal one; ack the normal one so
     * compaction has something to drop. */
    ASSERT_EQ(qdb_push(db, "q", NULL, 0), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", "data", 4), QDB_OK);
    ASSERT_EQ(qdb_push(db, "q", NULL, 0), QDB_OK);

    qdb_msg_t m = {0};
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);        /* pops msg 1 (empty) */
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    /* Two messages remain: msg 2 (normal) and msg 3 (empty). */
    qdb_stats_t st = {0};
    ASSERT_EQ(qdb_stats(db, &st), QDB_OK);
    ASSERT_EQ(st.pending_count, (uint64_t)2);

    /* Pop msg 2 (normal, len=4). */
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    ASSERT_EQ(m.len, (size_t)4);
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    /* Pop msg 3 (empty, len=0). */
    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_OK);
    ASSERT_EQ(m.len, (size_t)0);
    ASSERT_NULL(m.data);
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_pop(db, "q", &m), QDB_ERR_EMPTY);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 5. compact works with a 255-byte queue name
 * ---------------------------------------------------------------------- */

static void test_compact_max_name(void)
{
    char        name[QDB_QUEUE_NAME_MAX + 1];
    const char *path = "bnd_cpt_maxname.qdb";

    test_begin("compact works with 255-byte queue name");
    make_name(name, sizeof(name), QDB_QUEUE_NAME_MAX);
    cleanup(path);

    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, name, "keep", 4), QDB_OK);
    ASSERT_EQ(qdb_push(db, name, "drop", 4), QDB_OK);

    /* Ack the second message so compaction has something to discard. */
    qdb_msg_t m = {0};
    ASSERT_EQ(qdb_pop(db, name, &m), QDB_OK);       /* msg 1 */
    ASSERT_EQ(m.len, (size_t)4);
    qdb_msg_free(&m);
    ASSERT_EQ(qdb_pop(db, name, &m), QDB_OK);       /* msg 2 */
    ASSERT_EQ(qdb_ack(db, m.id, m.lease_id), QDB_OK);
    qdb_msg_free(&m);

    ASSERT_EQ(qdb_compact(db), QDB_OK);

    /* After compact: msg 1 is still leased, msg 2 is gone. */
    qdb_queue_stats_t qs = {0};
    ASSERT_EQ(qdb_queue_stats(db, name, &qs), QDB_OK);
    ASSERT_EQ(qs.leased_count,  (uint64_t)1);
    ASSERT_EQ(qs.pending_count, (uint64_t)0);

    qdb_close(db);
    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB boundary tests\n");
    printf("==================\n");

    test_empty_payload_roundtrip();
    test_empty_payload_reopen();
    test_max_name_roundtrip();
    test_max_name_queue_stats();
    test_max_name_reopen();
    test_overlimit_name_rejected();
    test_compact_empty_payload();
    test_compact_max_name();

    printf("==================\n");
    printf("Results: %d/%d passed",
           g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }
    printf("\n");
    return EXIT_SUCCESS;
}
