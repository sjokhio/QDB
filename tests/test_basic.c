/*
 * test_basic.c — basic smoke tests for the QDB public API
 *
 * These tests verify that the library links correctly and that the
 * utility functions behave as documented.  Tests for queue operations
 * (push / pop / ack) are written here as stubs; they will be filled in
 * once the storage engine is implemented.
 *
 * The test harness is a minimal, dependency-free framework: each test is
 * a function that calls ASSERT() macros.  Any failing assertion prints a
 * diagnostic and causes the process to exit with a non-zero status,
 * which CMake's ctest treats as a test failure.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Minimal test harness
 * ---------------------------------------------------------------------- */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define ASSERT(expr)                                                        \
    do {                                                                    \
        g_tests_run++;                                                      \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr); \
            g_tests_failed++;                                               \
        }                                                                   \
    } while (0)

#define ASSERT_EQ(a, b)  ASSERT((a) == (b))
#define ASSERT_NE(a, b)  ASSERT((a) != (b))
#define ASSERT_NULL(p)   ASSERT((p) == NULL)
#define ASSERT_STREQ(a, b) ASSERT(strcmp((a), (b)) == 0)

static void test_begin(const char *name)
{
    printf("  %-50s", name);
    fflush(stdout);
}

static void test_end(void)
{
    printf("ok\n");
}

/* -------------------------------------------------------------------------
 * Tests: version and error messages
 * ---------------------------------------------------------------------- */

static void test_version(void)
{
    test_begin("qdb_version returns non-null, non-empty string");
    const char *v = qdb_version();
    ASSERT(v != NULL);
    ASSERT(v[0] != '\0');
    test_end();
}

static void test_errmsg_ok(void)
{
    test_begin("qdb_errmsg(QDB_OK) returns non-null string");
    const char *msg = qdb_errmsg(QDB_OK);
    ASSERT(msg != NULL);
    ASSERT(msg[0] != '\0');
    test_end();
}

static void test_errmsg_all_codes(void)
{
    test_begin("qdb_errmsg returns non-null for all defined error codes");
    int codes[] = {
        QDB_OK,
        QDB_ERR_IO,
        QDB_ERR_CORRUPT,
        QDB_ERR_INVAL,
        QDB_ERR_EMPTY,
        QDB_ERR_NOENT,
        QDB_ERR_NOMEM,
        QDB_ERR_LOCKED,
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *msg = qdb_errmsg(codes[i]);
        ASSERT(msg != NULL);
        ASSERT(msg[0] != '\0');
    }
    test_end();
}

static void test_errmsg_unknown(void)
{
    test_begin("qdb_errmsg handles unknown error code without crashing");
    const char *msg = qdb_errmsg(-9999);
    ASSERT(msg != NULL);
    test_end();
}

/* -------------------------------------------------------------------------
 * Tests: qdb_open basic behaviour
 * ---------------------------------------------------------------------- */

static void test_open_creates_database(void)
{
    const char *path = "basic_test_open.qdb";
    test_begin("qdb_open creates a new database successfully");
    /* Clean up any leftover from a previous run. */
    (void)unlink(path);
    (void)unlink("basic_test_open.qdb-wal");
    (void)unlink("basic_test_open.qdb-lock");

    qdb_t *db = qdb_open(path);
    ASSERT(db != NULL);
    qdb_close(db);

    (void)unlink(path);
    (void)unlink("basic_test_open.qdb-wal");
    (void)unlink("basic_test_open.qdb-lock");
    test_end();
}

static void test_close_null_is_safe(void)
{
    test_begin("qdb_close(NULL) does not crash");
    qdb_close(NULL);  /* must be a no-op */
    ASSERT(1);        /* reaching here means no crash */
    test_end();
}

/* -------------------------------------------------------------------------
 * Tests: stub return codes
 * ---------------------------------------------------------------------- */

static void test_push_null_db_returns_inval(void)
{
    test_begin("qdb_push(NULL, ...) returns QDB_ERR_INVAL");
    int rc = qdb_push(NULL, "q", "data", 4);
    ASSERT_EQ(rc, QDB_ERR_INVAL);
    test_end();
}

static void test_pop_stub_returns_empty(void)
{
    test_begin("qdb_pop stub returns QDB_ERR_EMPTY (unimplemented)");
    qdb_msg_t msg = {0};
    int rc = qdb_pop(NULL, "q", &msg);
    ASSERT_EQ(rc, QDB_ERR_EMPTY);
    test_end();
}

static void test_ack_stub_returns_noent(void)
{
    test_begin("qdb_ack stub returns QDB_ERR_NOENT (unimplemented)");
    int rc = qdb_ack(NULL, 0);
    ASSERT_EQ(rc, QDB_ERR_NOENT);
    test_end();
}

/* -------------------------------------------------------------------------
 * Tests: compile-time constants
 * ---------------------------------------------------------------------- */

static void test_constants(void)
{
    test_begin("QDB_QUEUE_NAME_MAX and QDB_MSG_MAX_LEN are positive");
    ASSERT(QDB_QUEUE_NAME_MAX > 0);
    ASSERT(QDB_MSG_MAX_LEN > 0);
    test_end();
}

static void test_version_number(void)
{
    test_begin("QDB_VERSION_NUMBER macro is consistent with components");
    int computed = QDB_VERSION_MAJOR * 10000
                 + QDB_VERSION_MINOR * 100
                 + QDB_VERSION_PATCH;
    ASSERT_EQ(QDB_VERSION_NUMBER, computed);
    test_end();
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB basic tests\n");
    printf("===============\n");

    test_version();
    test_errmsg_ok();
    test_errmsg_all_codes();
    test_errmsg_unknown();
    test_open_creates_database();
    test_close_null_is_safe();
    test_push_null_db_returns_inval();
    test_pop_stub_returns_empty();
    test_ack_stub_returns_noent();
    test_constants();
    test_version_number();

    printf("===============\n");
    printf("Results: %d/%d passed", g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }

    printf("\n");
    return EXIT_SUCCESS;
}
