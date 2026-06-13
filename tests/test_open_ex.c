/*
 * test_open_ex.c — qdb_open_ex() and configurable lease timeout tests
 *
 * Tests that need an elapsed lease use force_expire() (setting expiry_us to
 * 1) rather than sleeping.  Other tests inspect the calculated expiry window
 * directly, keeping the suite deterministic and fast.
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_platform.h"

/* -------------------------------------------------------------------------
 * Minimal test harness (same pattern as all other test files)
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

static qdb_t *open_ex_fresh(const char *path, const qdb_open_opts_t *opts)
{
    cleanup(path);
    return qdb_open_ex(path, opts);
}

/* Force a lease to appear already-expired (same technique as test_expire.c). */
static void force_expire(qdb_t *db, uint64_t lease_id)
{
    struct qdb__lease *l = qdb__lease_get(db->state, lease_id);
    if (l) {
        l->expiry_us = 1; /* epoch + 1 µs — always in the past */
    }
}

/* -------------------------------------------------------------------------
 * Test 1: qdb_open_ex(path, NULL) behaves exactly like qdb_open()
 * ---------------------------------------------------------------------- */

static void test_open_ex_null_opts(void)
{
    test_begin("qdb_open_ex(NULL opts): opens, push/pop/ack cycle works");

    const char *path = "oex_null.qdb";
    qdb_t *db = open_ex_fresh(path, NULL);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "hello", 5), QDB_OK);

    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    ASSERT_EQ(msg.len, (size_t)5);
    ASSERT_EQ(memcmp(msg.data, "hello", 5), 0);

    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);

    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_ERR_EMPTY);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 2: zero-initialised opts → same default timeout as qdb_open()
 * ---------------------------------------------------------------------- */

static void test_open_ex_zero_opts(void)
{
    test_begin("qdb_open_ex({0} opts): uses QDB_DEFAULT_LEASE_US");

    const char *path = "oex_zero.qdb";
    qdb_open_opts_t opts = {0};
    qdb_t *db = open_ex_fresh(path, &opts);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(db->lease_timeout_us, QDB_DEFAULT_LEASE_US);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 3: custom timeout stored correctly on db handle
 * ---------------------------------------------------------------------- */

static void test_open_ex_custom_timeout_stored(void)
{
    test_begin("qdb_open_ex(lease_timeout_s=120): db->lease_timeout_us==120M");

    const char *path = "oex_stored.qdb";
    qdb_open_opts_t opts = {0};
    opts.lease_timeout_s = 120;
    qdb_t *db = open_ex_fresh(path, &opts);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(db->lease_timeout_us, UINT64_C(120000000));

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 4: qdb_open() stores QDB_DEFAULT_LEASE_US (backward compat)
 * ---------------------------------------------------------------------- */

static void test_open_backward_compat_timeout(void)
{
    test_begin("qdb_open(): stores QDB_DEFAULT_LEASE_US (backward compat)");

    const char *path = "oex_compat_timeout.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(db->lease_timeout_us, QDB_DEFAULT_LEASE_US);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 5: lease expiry in message state reflects custom timeout window
 *
 * Strategy: record wall-clock bounds around qdb_pop(), then assert the
 * stored expiry_us falls within [before + timeout, after + timeout].
 * The bounds check avoids waiting for the lease to expire.
 * ---------------------------------------------------------------------- */

static void test_open_ex_expiry_reflects_timeout(void)
{
    test_begin("pop after custom timeout: m->lease_expiry_us ≈ now + timeout");

    const char *path = "oex_expiry.qdb";
    qdb_open_opts_t opts = {0};
    opts.lease_timeout_s = 60; /* 60 s = 60 000 000 µs */
    qdb_t *db = open_ex_fresh(path, &opts);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    uint64_t t_before = qdb__time_us();
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    uint64_t t_after = qdb__time_us();

    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);

    uint64_t timeout_us = UINT64_C(60000000);
    ASSERT(m->lease_expiry_us >= t_before + timeout_us);
    ASSERT(m->lease_expiry_us <= t_after  + timeout_us);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 6: default timeout (NULL opts) expiry window matches QDB_DEFAULT_LEASE_US
 * ---------------------------------------------------------------------- */

static void test_open_default_expiry_window(void)
{
    test_begin("qdb_open(): lease expiry ≈ now + QDB_DEFAULT_LEASE_US");

    const char *path = "oex_defexpiry.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    uint64_t t_before = qdb__time_us();
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
    uint64_t t_after = qdb__time_us();

    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);

    ASSERT(m->lease_expiry_us >= t_before + QDB_DEFAULT_LEASE_US);
    ASSERT(m->lease_expiry_us <= t_after  + QDB_DEFAULT_LEASE_US);

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 7: custom timeout + force_expire → message returns to PENDING
 *
 * The custom timeout is irrelevant to qdb_process_expired_leases(), which
 * only compares stored expiry_us against now.  This test verifies the full
 * expiry path still works when a custom timeout was set at open time.
 * ---------------------------------------------------------------------- */

static void test_open_ex_custom_timeout_expires(void)
{
    test_begin("custom timeout + force_expire: message returns to PENDING");

    const char *path = "oex_customexp.qdb";
    qdb_open_opts_t opts = {0};
    opts.lease_timeout_s = 3600; /* 1 hour — will never naturally expire */
    qdb_t *db = open_ex_fresh(path, &opts);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "payload", 7), QDB_OK);
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    /* Verify the configured lease is still active before forcing expiry. */
    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT(m->lease_expiry_us > qdb__time_us() + UINT64_C(3500000000)); /* > 3500 s away */

    /* Force the lease into the past and expire it. */
    force_expire(db, msg.lease_id);
    ASSERT_EQ(qdb_process_expired_leases(db), 1);

    ASSERT_EQ((int)m->state, (int)QDB_MSG_STATE_PENDING);
    ASSERT_EQ(m->lease_id,        (uint64_t)0);
    ASSERT_EQ(m->lease_expiry_us, (uint64_t)0);

    /* Queue counts updated */
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
 * Test 8: two opens with different timeouts grant different expiry windows
 * ---------------------------------------------------------------------- */

static void test_open_ex_different_timeouts(void)
{
    test_begin("two opens with different timeouts produce different expiry windows");

    const char *path_a = "oex_diffa.qdb";
    const char *path_b = "oex_diffb.qdb";

    qdb_open_opts_t short_opts = {0};
    short_opts.lease_timeout_s = 10;

    qdb_open_opts_t long_opts = {0};
    long_opts.lease_timeout_s = 7200;

    qdb_t *dba = open_ex_fresh(path_a, &short_opts);
    qdb_t *dbb = open_ex_fresh(path_b, &long_opts);
    ASSERT_NOTNULL(dba);
    ASSERT_NOTNULL(dbb);

    ASSERT_EQ(dba->lease_timeout_us, UINT64_C(10000000));
    ASSERT_EQ(dbb->lease_timeout_us, UINT64_C(7200000000));
    ASSERT_NE(dba->lease_timeout_us, dbb->lease_timeout_us);

    /* Pop from each and verify expiry difference ≈ (7200 - 10) seconds */
    ASSERT_EQ(qdb_push(dba, "q", "x", 1), QDB_OK);
    ASSERT_EQ(qdb_push(dbb, "q", "x", 1), QDB_OK);

    qdb_msg_t ma = {0}, mb = {0};
    ASSERT_EQ(qdb_pop(dba, "q", &ma), QDB_OK);
    ASSERT_EQ(qdb_pop(dbb, "q", &mb), QDB_OK);

    struct qdb__msg *msa = qdb__msg_get(dba->state, ma.id);
    struct qdb__msg *msb = qdb__msg_get(dbb->state, mb.id);
    ASSERT_NOTNULL(msa);
    ASSERT_NOTNULL(msb);

    /* msb's expiry must be substantially larger than msa's. */
    ASSERT(msb->lease_expiry_us > msa->lease_expiry_us + UINT64_C(7000000000)); /* > 7000 s diff */

    qdb_msg_free(&ma);
    qdb_msg_free(&mb);
    qdb_close(dba);
    qdb_close(dbb);
    cleanup(path_a);
    cleanup(path_b);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 9: NULL path → returns NULL for both qdb_open() and qdb_open_ex()
 * ---------------------------------------------------------------------- */

static void test_open_ex_null_path(void)
{
    test_begin("NULL path returns NULL for qdb_open() and qdb_open_ex()");

    ASSERT_NULL(qdb_open(NULL));
    ASSERT_NULL(qdb_open_ex(NULL, NULL));

    qdb_open_opts_t opts = {0};
    opts.lease_timeout_s = 60;
    ASSERT_NULL(qdb_open_ex(NULL, &opts));

    test_end();
}

/* -------------------------------------------------------------------------
 * Test 10: UINT32_MAX timeout accepted, no overflow in microsecond value
 * ---------------------------------------------------------------------- */

static void test_open_ex_max_timeout(void)
{
    test_begin("UINT32_MAX lease_timeout_s accepted, no uint64 overflow");

    const char *path = "oex_maxto.qdb";
    qdb_open_opts_t opts = {0};
    opts.lease_timeout_s = UINT32_MAX; /* ≈ 136 years */
    qdb_t *db = open_ex_fresh(path, &opts);
    ASSERT_NOTNULL(db);

    uint64_t expected_us = (uint64_t)UINT32_MAX * UINT64_C(1000000);
    ASSERT_EQ(db->lease_timeout_us, expected_us);

    /* pop succeeds; expiry_us = now + expected_us, still fits in uint64 */
    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    qdb_msg_t msg = {0};
    ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);

    struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
    ASSERT_NOTNULL(m);
    ASSERT(m->lease_expiry_us > qdb__time_us()); /* expiry is in the future */

    qdb_msg_free(&msg);
    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 11: reopening a database with different timeout applies new timeout
 * ---------------------------------------------------------------------- */

static void test_open_ex_reopen_new_timeout(void)
{
    test_begin("reopen with different timeout: new pops use new timeout");

    const char *path = "oex_reopen.qdb";
    cleanup(path);

    /* Session 1: open with 10 s timeout, push, close. */
    {
        qdb_open_opts_t opts = {0};
        opts.lease_timeout_s = 10;
        qdb_t *db = qdb_open_ex(path, &opts);
        ASSERT_NOTNULL(db);
        ASSERT_EQ(qdb_push(db, "q", "msg", 3), QDB_OK);
        qdb_close(db);
    }

    /* Session 2: reopen with 900 s timeout; pop gets the new timeout. */
    {
        qdb_open_opts_t opts = {0};
        opts.lease_timeout_s = 900;
        qdb_t *db = qdb_open_ex(path, &opts);
        ASSERT_NOTNULL(db);
        ASSERT_EQ(db->lease_timeout_us, UINT64_C(900000000));

        uint64_t t_before = qdb__time_us();
        qdb_msg_t msg = {0};
        ASSERT_EQ(qdb_pop(db, "q", &msg), QDB_OK);
        uint64_t t_after = qdb__time_us();

        struct qdb__msg *m = qdb__msg_get(db->state, msg.id);
        ASSERT_NOTNULL(m);
        ASSERT(m->lease_expiry_us >= t_before + UINT64_C(900000000));
        ASSERT(m->lease_expiry_us <= t_after  + UINT64_C(900000000));

        qdb_msg_free(&msg);
        qdb_close(db);
    }

    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB qdb_open_ex() tests\n");
    printf("=======================\n");

    test_open_ex_null_opts();
    test_open_ex_zero_opts();
    test_open_ex_custom_timeout_stored();
    test_open_backward_compat_timeout();
    test_open_ex_expiry_reflects_timeout();
    test_open_default_expiry_window();
    test_open_ex_custom_timeout_expires();
    test_open_ex_different_timeouts();
    test_open_ex_null_path();
    test_open_ex_max_timeout();
    test_open_ex_reopen_new_timeout();

    printf("=======================\n");
    printf("Results: %d/%d passed", g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }

    printf("\n");
    return EXIT_SUCCESS;
}
