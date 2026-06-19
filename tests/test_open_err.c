/*
 * test_open_err.c — qdb_open_err() tests
 *
 * Verifies that qdb_open_err() surfaces the correct QDB_ERR_* code at each
 * failure path, that NULL out_err is always safe, and that qdb_open() and
 * qdb_open_ex() remain functional wrappers.
 *
 * The LOCKED test uses fork(2) and is skipped on Windows.
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

#define QDB_TEST_PLATFORM_RAW_IO
#include "test_platform.h"

#include "../src/qdb_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <signal.h>
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
    qdb_t *db = qdb_open(path);
    return db;
}

/* -------------------------------------------------------------------------
 * Test 1: NULL path → QDB_ERR_INVAL
 * ---------------------------------------------------------------------- */

static void test_null_path(void)
{
    test_begin("NULL path: returns NULL + QDB_ERR_INVAL");

    int rc = QDB_OK;
    qdb_t *db = qdb_open_err(NULL, NULL, &rc);
    ASSERT_NULL(db);
    ASSERT_EQ(rc, QDB_ERR_INVAL);

    test_end();
}

/* -------------------------------------------------------------------------
 * Test 2: NULL out_err is safe on success path
 * ---------------------------------------------------------------------- */

static void test_null_out_err_safe(void)
{
    test_begin("NULL out_err on success: returns valid handle, no crash");

    const char *path = "oerr_null_outerr.qdb";
    qdb_t *db = open_fresh(path);   /* create via qdb_open so path exists */
    qdb_close(db);
    cleanup(path);

    /* Open fresh with NULL out_err — must not crash and must succeed. */
    cleanup(path);
    db = qdb_open_err(path, NULL, NULL);
    ASSERT_NOTNULL(db);
    qdb_close(db);

    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 3: success → *out_err = QDB_OK
 * ---------------------------------------------------------------------- */

static void test_success_sets_ok(void)
{
    test_begin("success: *out_err set to QDB_OK");

    const char *path = "oerr_success.qdb";
    int rc = QDB_ERR_IO;   /* pre-poison to verify it is overwritten */
    qdb_t *db = qdb_open_err(path, NULL, &rc);
    ASSERT_NOTNULL(db);
    ASSERT_EQ(rc, QDB_OK);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 4: nonexistent parent directory → QDB_ERR_IO
 * ---------------------------------------------------------------------- */

static void test_bad_directory_io(void)
{
    test_begin("nonexistent directory: returns NULL + QDB_ERR_IO");

    int rc = QDB_OK;
    qdb_t *db = qdb_open_err("nonexistent_dir/sub/x.qdb", NULL, &rc);
    ASSERT_NULL(db);
    ASSERT_EQ(rc, QDB_ERR_IO);

    test_end();
}

/* -------------------------------------------------------------------------
 * Test 5: database locked by another process → QDB_ERR_LOCKED
 * ---------------------------------------------------------------------- */

static void test_locked(void)
{
    test_begin("locked by other process: returns NULL + QDB_ERR_LOCKED");

#if defined(_WIN32)
    /* fork(2) is not available; skip the assertion, count the test as ok. */
    test_end();
    return;
#else
    const char *path = "oerr_locked.qdb";
    cleanup(path);

    int child_to_parent[2];   /* child writes "ready" here */
    int parent_to_child[2];   /* parent writes "done" here to release child */

    ASSERT(pipe(child_to_parent) == 0);
    ASSERT(pipe(parent_to_child) == 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);

    if (pid == 0) {
        /* Child: open db (acquire lock), signal parent, wait for release. */
        close(child_to_parent[0]);
        close(parent_to_child[1]);

        qdb_t *db = qdb_open(path);
        char ready = (char)(db ? 1 : 0);
        (void)write(child_to_parent[1], &ready, 1);
        close(child_to_parent[1]);

        char done = 0;
        (void)read(parent_to_child[0], &done, 1);
        close(parent_to_child[0]);

        if (db) {
            qdb_close(db);
        }
        _exit(0);
    }

    /* Parent: wait for child to signal that it holds the lock. */
    close(child_to_parent[1]);
    close(parent_to_child[0]);

    char ready = 0;
    (void)read(child_to_parent[0], &ready, 1);
    close(child_to_parent[0]);

    ASSERT_EQ((int)ready, 1);

    /* Attempt to open the locked database. */
    int rc = QDB_OK;
    qdb_t *db = qdb_open_err(path, NULL, &rc);
    ASSERT_NULL(db);
    ASSERT_EQ(rc, QDB_ERR_LOCKED);

    /* Signal child to release the lock and exit. */
    char done = 1;
    (void)write(parent_to_child[1], &done, 1);
    close(parent_to_child[1]);

    int status;
    waitpid(pid, &status, 0);

    cleanup(path);
    test_end();
#endif
}

/* -------------------------------------------------------------------------
 * Test 6: corrupt file → QDB_ERR_CORRUPT
 * ---------------------------------------------------------------------- */

static void test_corrupt_file(void)
{
    test_begin("corrupt file (bad magic): returns NULL + QDB_ERR_CORRUPT");

    const char *path = "oerr_corrupt.qdb";

    /* Create a valid database. */
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);
    qdb_close(db);

    /* Corrupt the first byte of the file header (magic bytes). */
    uint8_t bad = 0xFFu;
    ASSERT_EQ(qdb_test_raw_write_at(path, 0, &bad, 1), 0);

    /* Re-open must fail with CORRUPT. */
    int rc = QDB_OK;
    db = qdb_open_err(path, NULL, &rc);
    ASSERT_NULL(db);
    ASSERT_EQ(rc, QDB_ERR_CORRUPT);

    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 7: qdb_open() is still a working wrapper
 * ---------------------------------------------------------------------- */

static void test_backward_compat_open(void)
{
    test_begin("qdb_open() wrapper: returns valid handle");

    const char *path = "oerr_compat_open.qdb";
    qdb_t *db = open_fresh(path);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 8: qdb_open_ex() is still a working wrapper
 * ---------------------------------------------------------------------- */

static void test_backward_compat_open_ex(void)
{
    test_begin("qdb_open_ex() wrapper: returns valid handle");

    const char *path = "oerr_compat_openex.qdb";
    cleanup(path);
    qdb_t *db = qdb_open_ex(path, NULL);
    ASSERT_NOTNULL(db);

    ASSERT_EQ(qdb_push(db, "q", "y", 1), QDB_OK);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 9: qdb_open_err() with non-NULL opts → success + QDB_OK
 * ---------------------------------------------------------------------- */

static void test_open_err_with_opts(void)
{
    test_begin("qdb_open_err() with opts: returns handle + QDB_OK");

    const char *path = "oerr_with_opts.qdb";
    cleanup(path);

    qdb_open_opts_t opts = {0};
    opts.lease_timeout_s = 60;

    int rc = QDB_ERR_IO;   /* pre-poison */
    qdb_t *db = qdb_open_err(path, &opts, &rc);
    ASSERT_NOTNULL(db);
    ASSERT_EQ(rc, QDB_OK);
    ASSERT_EQ(db->lease_timeout_us, UINT64_C(60000000));

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Test 10: qdb_open_err(path, NULL, NULL) → same as qdb_open()
 * ---------------------------------------------------------------------- */

static void test_open_err_null_null(void)
{
    test_begin("qdb_open_err(path, NULL, NULL): behaves like qdb_open()");

    const char *path = "oerr_null_null.qdb";
    cleanup(path);

    qdb_t *db = qdb_open_err(path, NULL, NULL);
    ASSERT_NOTNULL(db);
    ASSERT_EQ(db->lease_timeout_us, QDB_DEFAULT_LEASE_US);

    qdb_close(db);
    cleanup(path);
    test_end();
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB qdb_open_err() tests\n");
    printf("========================\n");

    test_null_path();
    test_null_out_err_safe();
    test_success_sets_ok();
    test_bad_directory_io();
    test_locked();
    test_corrupt_file();
    test_backward_compat_open();
    test_backward_compat_open_ex();
    test_open_err_with_opts();
    test_open_err_null_null();

    printf("========================\n");
    printf("Results: %d/%d passed", g_tests_run - g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }

    printf("\n");
    return EXIT_SUCCESS;
}
