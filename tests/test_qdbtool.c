/*
 * test_qdbtool.c — integration tests for the qdbtool CLI
 *
 * Each test creates a QDB database via the public API, releases the lock
 * (qdb_close), then invokes qdbtool as a subprocess and checks the exit
 * code and key substrings in the output.  Output format details (column
 * alignment, exact byte counts) are intentionally not checked.
 *
 * The path to the built qdbtool executable is passed in via the
 * QDBTOOL_EXE preprocessor definition, set by CMake.
 *
 * SPDX-License-Identifier: MIT
 */

/* -------------------------------------------------------------------------
 * Platform preamble
 * ---------------------------------------------------------------------- */

#if defined(_WIN32)
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS
#  endif
#  ifndef _CRT_NONSTDC_NO_DEPRECATE
#    define _CRT_NONSTDC_NO_DEPRECATE
#  endif
#else
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <sys/wait.h>
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

/* -------------------------------------------------------------------------
 * Subprocess helpers
 * ---------------------------------------------------------------------- */

/*
 * run_tool — run qdbtool with the given argument string and capture combined
 * stdout+stderr into out_buf.  Returns the process exit code.
 *
 * The argument string is passed directly to popen(); the test caller is
 * responsible for constructing a safe command (paths are simple filenames in
 * the working directory with no special characters).
 */
static int run_tool(const char *args, char *out_buf, size_t out_sz)
{
    char  cmd[4096];
    FILE *fp;
    int   status;

    if (args && *args)
        snprintf(cmd, sizeof(cmd), "\"%s\" %s 2>&1", QDBTOOL_EXE, args);
    else
        snprintf(cmd, sizeof(cmd), "\"%s\" 2>&1", QDBTOOL_EXE);

#ifdef _WIN32
    fp = _popen(cmd, "r");
#else
    fp = popen(cmd, "r");
#endif
    if (!fp) { return -1; }

    /* Drain the pipe completely so the child process can exit cleanly. */
    if (out_buf && out_sz > 0) {
        size_t n = fread(out_buf, 1, out_sz - 1, fp);
        out_buf[n] = '\0';
        /* Drain any remainder that did not fit in the caller's buffer. */
        {
            char   drain[256];
            size_t nread;
            do {
                nread = fread(drain, 1, sizeof(drain), fp);
            } while (nread > 0);
        }
    } else {
        char   drain[256];
        size_t nread;
        do {
            nread = fread(drain, 1, sizeof(drain), fp);
        } while (nread > 0);
    }

#ifdef _WIN32
    status = _pclose(fp);
    return status;
#else
    status = pclose(fp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* Return 1 if needle appears in haystack. */
static int str_contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

/* -------------------------------------------------------------------------
 * Helper: make a qdbtool argument string with the given flags and path.
 * The caller supplies a fixed-size buffer.
 * ---------------------------------------------------------------------- */

#define ARGS(buf, ...) snprintf((buf), sizeof(buf), __VA_ARGS__)

/* -------------------------------------------------------------------------
 * 1. info — basic human-readable output
 * ---------------------------------------------------------------------- */

static void test_info_basic(void)
{
    const char *path = "tc_info.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;
    qdb_msg_t   msg = {0};

    test_begin("info: human-readable output");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "b", 1), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "jobs", &msg),    QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);
    qdb_close(db);

    ARGS(args, "info %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "Pending:"));
    ASSERT(str_contains(out, "Acked:"));
    ASSERT(str_contains(out, "Compact advised:"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 2. info --json
 * ---------------------------------------------------------------------- */

static void test_info_json(void)
{
    const char *path = "tc_info_json.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("info --json: JSON object with required keys");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "tasks", "x", 1), QDB_OK);
    qdb_close(db);

    ARGS(args, "info --json %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "\"pending\""));
    ASSERT(str_contains(out, "\"acked\""));
    ASSERT(str_contains(out, "\"queues\""));
    ASSERT(str_contains(out, "\"compact_advised\""));
    ASSERT(str_contains(out, "\"size_bytes\""));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 3. list — basic output (one name per line)
 * ---------------------------------------------------------------------- */

static void test_list_basic(void)
{
    const char *path = "tc_list.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("list: queue names, one per line");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "emails", "e", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs",   "j", 1), QDB_OK);
    qdb_close(db);

    ARGS(args, "list %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "emails"));
    ASSERT(str_contains(out, "jobs"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 4. list --json
 * ---------------------------------------------------------------------- */

static void test_list_json(void)
{
    const char *path = "tc_list_json.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("list --json: JSON string array");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "emails", "e", 1), QDB_OK);
    qdb_close(db);

    ARGS(args, "list --json %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "["));
    ASSERT(str_contains(out, "emails"));
    ASSERT(str_contains(out, "]"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 5. stats — all queues
 * ---------------------------------------------------------------------- */

static void test_stats_all(void)
{
    const char *path = "tc_stats_all.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("stats (all queues): queue names and counts present");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "alpha", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "beta",  "b", 1), QDB_OK);
    qdb_close(db);

    ARGS(args, "stats %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "alpha"));
    ASSERT(str_contains(out, "beta"));
    ASSERT(str_contains(out, "Pending"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 6. stats — single queue
 * ---------------------------------------------------------------------- */

static void test_stats_queue(void)
{
    const char *path = "tc_stats_q.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("stats <queue>: per-queue output");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "jobs", "j1", 2), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "j2", 2), QDB_OK);
    qdb_close(db);

    ARGS(args, "stats %s jobs", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "jobs"));
    ASSERT(str_contains(out, "Pending:"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 7. stats --json
 * ---------------------------------------------------------------------- */

static void test_stats_json(void)
{
    const char *path = "tc_stats_json.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("stats --json: JSON output with queue/pending/leased/acked");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "work", "w", 1), QDB_OK);
    qdb_close(db);

    ARGS(args, "stats --json %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "\"queue\""));
    ASSERT(str_contains(out, "\"pending\""));
    ASSERT(str_contains(out, "\"leased\""));
    ASSERT(str_contains(out, "\"acked\""));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 8. compact — not recommended (no output of "Before:")
 * ---------------------------------------------------------------------- */

static void test_compact_not_recommended(void)
{
    const char *path = "tc_compact_no.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("compact: not recommended -> exit 0, informational message");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "b", 1), QDB_OK);
    /* No acks: acked_count == 0, so not recommended */
    qdb_close(db);

    ARGS(args, "compact %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "not needed"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 9. compact — recommended (acked > live)
 * ---------------------------------------------------------------------- */

static void test_compact_recommended(void)
{
    const char *path = "tc_compact_yes.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;
    qdb_msg_t   msg = {0};

    test_begin("compact: recommended -> compacts, prints Before:/After:");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    /* Push 3, ack 2, leave 1 pending: acked(2) > live(1) */
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

    qdb_close(db);

    ARGS(args, "compact %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "Before:"));
    ASSERT(str_contains(out, "After:"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 10. compact --dry-run
 * ---------------------------------------------------------------------- */

static void test_compact_dry_run(void)
{
    const char *path = "tc_compact_dry.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;
    qdb_msg_t   msg = {0};

    test_begin("compact --dry-run: reports recommendation, no changes");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    ASSERT_EQ(qdb_push(db, "jobs", "b", 1), QDB_OK);
    ASSERT_EQ(qdb_pop(db, "jobs", &msg), QDB_OK);
    ASSERT_EQ(qdb_ack(db, msg.id, msg.lease_id), QDB_OK);
    qdb_msg_free(&msg);
    /* acked(1) == live(1): not recommended yet, but dry-run still reports */
    qdb_close(db);

    ARGS(args, "compact --dry-run %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "dry run"));
    ASSERT(str_contains(out, "Current size:"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 11. compact --force (runs even when not recommended)
 * ---------------------------------------------------------------------- */

static void test_compact_force(void)
{
    const char *path = "tc_compact_force.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("compact --force: compacts regardless of recommendation");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, "jobs", "a", 1), QDB_OK);
    /* No acks: acked == 0, not recommended; --force overrides */
    qdb_close(db);

    ARGS(args, "compact --force %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "Before:"));
    ASSERT(str_contains(out, "After:"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 12. verify — valid database
 * ---------------------------------------------------------------------- */

static void test_verify_valid(void)
{
    const char *path = "tc_verify.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("verify: valid database -> exit 0, 'ok' in output");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }
    ASSERT_EQ(qdb_push(db, "q", "x", 1), QDB_OK);
    qdb_close(db);

    ARGS(args, "verify %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "ok"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 13. verify --json
 * ---------------------------------------------------------------------- */

static void test_verify_json(void)
{
    const char *path = "tc_verify_json.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("verify --json: {\"ok\":true} on valid database");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }
    qdb_close(db);

    ARGS(args, "verify --json %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    ASSERT(str_contains(out, "\"ok\""));
    ASSERT(str_contains(out, "true"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 14. Missing file -> exit 1
 *
 * qdbtool now calls file_exists() before qdb_open(), so a bare missing
 * path correctly returns exit 1 with "file not found" rather than creating
 * a new database.
 * ---------------------------------------------------------------------- */

static void test_missing_file(void)
{
    const char *path = "tc_missing_xyz987654.qdb";
    char        args[512];
    char        out[2048];
    int         rc;

    test_begin("missing file: exit 1, 'file not found' message");

    ARGS(args, "info %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 1);
    ASSERT(str_contains(out, "not found"));

    test_end();
}

/* -------------------------------------------------------------------------
 * 15. Unknown command -> exit 2
 * ---------------------------------------------------------------------- */

static void test_unknown_command(void)
{
    char out[512];
    int  rc;

    test_begin("unknown command: exit 2");

    rc = run_tool("badcommand somefile.qdb", out, sizeof(out));

    ASSERT_EQ(rc, 2);

    test_end();
}

/* -------------------------------------------------------------------------
 * 16. No arguments -> exit 2
 * ---------------------------------------------------------------------- */

static void test_no_args(void)
{
    char out[512];
    int  rc;

    test_begin("no arguments: exit 2");

    rc = run_tool("", out, sizeof(out));

    ASSERT_EQ(rc, 2);

    test_end();
}

/* -------------------------------------------------------------------------
 * 17. JSON escaping — queue name containing a double-quote character
 *
 * Creates a queue with a name that contains ", then checks that list --json
 * produces \" in the output (RFC 8259 §7 mandatory escape).
 * ---------------------------------------------------------------------- */

static void test_json_escape_quote(void)
{
    const char *path      = "tc_json_esc_quote.qdb";
    /* Queue name: job"name */
    const char *qname     = "job\"name";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("JSON escape: double-quote in queue name -> \\\"");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, qname, "v", 1), QDB_OK);
    qdb_close(db);

    ARGS(args, "list --json %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    /* Escaped form \" must appear in the JSON output */
    ASSERT(str_contains(out, "\\\""));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 18. JSON escaping — queue name containing a backslash character
 *
 * Creates a queue with a name that contains \, then checks that list --json
 * produces \\ in the output (RFC 8259 §7 mandatory escape).
 * ---------------------------------------------------------------------- */

static void test_json_escape_backslash(void)
{
    const char *path      = "tc_json_esc_bs.qdb";
    /* Queue name: path\name */
    const char *qname     = "path\\name";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("JSON escape: backslash in queue name -> \\\\");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }

    ASSERT_EQ(qdb_push(db, qname, "v", 1), QDB_OK);
    qdb_close(db);

    ARGS(args, "list --json %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 0);
    /* Escaped form \\ must appear in the JSON output */
    ASSERT(str_contains(out, "\\\\"));

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 19. Per-command flag validation: --force rejected on info -> exit 2
 * ---------------------------------------------------------------------- */

static void test_invalid_flag_rejected(void)
{
    const char *path = "tc_flag_reject.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("invalid flag on info (--force): exit 2");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }
    qdb_close(db);

    ARGS(args, "info --force %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 2);

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 20. Per-command flag validation: --json rejected on compact -> exit 2
 * ---------------------------------------------------------------------- */

static void test_json_rejected_on_compact(void)
{
    const char *path = "tc_compact_json_reject.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("--json on compact: exit 2");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }
    qdb_close(db);

    ARGS(args, "compact --json %s", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 2);

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * 21. Extra positional argument rejected on info -> exit 2
 * ---------------------------------------------------------------------- */

static void test_extra_arg_rejected(void)
{
    const char *path = "tc_extra_arg.qdb";
    char        args[512];
    char        out[2048];
    int         rc;
    qdb_t      *db;

    test_begin("extra positional arg on info: exit 2");
    cleanup(path);

    db = qdb_open(path);
    ASSERT_NOTNULL(db);
    if (!db) { test_end(); cleanup(path); return; }
    qdb_close(db);

    /* info takes exactly one positional arg (path); the second is rejected */
    ARGS(args, "info %s extra_arg", path);
    rc = run_tool(args, out, sizeof(out));

    ASSERT_EQ(rc, 2);

    test_end();
    cleanup(path);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB qdbtool integration tests\n");
    printf("==============================\n");

    test_info_basic();
    test_info_json();
    test_list_basic();
    test_list_json();
    test_stats_all();
    test_stats_queue();
    test_stats_json();
    test_compact_not_recommended();
    test_compact_recommended();
    test_compact_dry_run();
    test_compact_force();
    test_verify_valid();
    test_verify_json();
    test_missing_file();
    test_unknown_command();
    test_no_args();
    test_json_escape_quote();
    test_json_escape_backslash();
    test_invalid_flag_rejected();
    test_json_rejected_on_compact();
    test_extra_arg_rejected();

    printf("==============================\n");
    printf("Results: %d/%d passed",
           g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }
    printf("\n");
    return EXIT_SUCCESS;
}
