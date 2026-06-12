/*
 * bench_push_pop.c — push / pop+ack throughput benchmark
 *
 * Measures sustained single-threaded throughput for the two hot paths:
 *   1. push   — append a message to the queue (2× fsync per call)
 *   2. pop+ack — pop + acknowledge a message (4× fsync per pair)
 *
 * Usage:  bench_push_pop [N]
 *   N  number of messages (default: 1000)
 *
 * The benchmark writes to a temporary file in the current directory and
 * removes it on exit.  Results are printed to stdout.
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <time.h>
#endif

#include "qdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Portable wall-clock timer (seconds)
 * ---------------------------------------------------------------------- */

#if defined(_WIN32)
static double now_sec(void)
{
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
}
#else
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
}
#endif

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void die(const char *msg, int rc)
{
    fprintf(stderr, "fatal: %s: %s\n", msg, qdb_errmsg(rc));
    exit(EXIT_FAILURE);
}

static void cleanup(const char *path)
{
#if defined(_WIN32)
    DeleteFileA(path);
    char s[512];
    size_t n = strlen(path);
    if (n + 6 < sizeof(s)) {
        memcpy(s, path, n); memcpy(s+n, "-wal",  5); DeleteFileA(s);
        memcpy(s, path, n); memcpy(s+n, "-lock", 6); DeleteFileA(s);
    }
#else
    remove(path);
    char s[512];
    size_t n = strlen(path);
    if (n + 6 < sizeof(s)) {
        memcpy(s, path, n); memcpy(s+n, "-wal",  5); remove(s);
        memcpy(s, path, n); memcpy(s+n, "-lock", 6); remove(s);
    }
#endif
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int n = (argc > 1) ? atoi(argv[1]) : 1000;
    if (n <= 0) {
        fprintf(stderr, "usage: bench_push_pop [N]\n");
        return EXIT_FAILURE;
    }

    const char *path    = "bench_push_pop.qdb";
    const char *queue   = "bench";
    const char *payload = "hello from bench";
    size_t      plen    = strlen(payload);

    cleanup(path);

    qdb_t *db = qdb_open(path);
    if (!db) {
        fprintf(stderr, "fatal: qdb_open failed\n");
        return EXIT_FAILURE;
    }

    printf("QDB push/pop+ack benchmark  (N=%d, payload=%zu bytes)\n\n", n, plen);

    /* ── Phase 1: push ─────────────────────────────────────────────── */

    double t0 = now_sec();

    for (int i = 0; i < n; i++) {
        int rc = qdb_push(db, queue, payload, plen);
        if (rc != QDB_OK) { die("qdb_push", rc); }
    }

    double t1     = now_sec();
    double push_s = t1 - t0;

    /* ── Phase 2: pop + ack ────────────────────────────────────────── */

    for (int i = 0; i < n; i++) {
        qdb_msg_t msg = {0};
        int rc = qdb_pop(db, queue, &msg);
        if (rc != QDB_OK) { die("qdb_pop", rc); }
        rc = qdb_ack(db, msg.id, msg.lease_id);
        if (rc != QDB_OK) { die("qdb_ack", rc); }
        qdb_msg_free(&msg);
    }

    double t2    = now_sec();
    double ack_s = t2 - t1;

    /* ── Results ───────────────────────────────────────────────────── */

    printf("push       %5d msgs  %6.3f s  %8.0f msg/s\n",
           n, push_s, push_s > 0.0 ? (double)n / push_s : 0.0);
    printf("pop+ack    %5d msgs  %6.3f s  %8.0f msg/s\n",
           n, ack_s,  ack_s  > 0.0 ? (double)n / ack_s  : 0.0);
    printf("total      %5d msgs  %6.3f s  %8.0f msg/s\n",
           n * 2, push_s + ack_s,
           (push_s + ack_s) > 0.0 ? (double)(n * 2) / (push_s + ack_s) : 0.0);

    qdb_close(db);
    cleanup(path);
    return EXIT_SUCCESS;
}
