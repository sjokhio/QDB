/*
 * job_worker.c — job queue example
 *
 * Demonstrates the complete QDB lifecycle:
 *   - Opening a queue database
 *   - Pushing jobs onto a named queue
 *   - A worker loop that pops jobs, processes them, and ACKs or NACKs
 *   - Calling qdb_process_expired_leases() to recover stale in-progress jobs
 *   - Clean shutdown
 *
 * Build:  cmake --build build  (included automatically with examples)
 * Run:    ./build/examples/example_job_worker
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Simulated job processor
 *
 * In a real application this would do actual work — resize an image, send
 * an email, call an external API, etc.  Here we just print the job name.
 *
 * We simulate a transient failure: jobs whose name contains "flaky" fail on
 * their first delivery attempt (retry_count == 0) and succeed on retry.
 * The retry_count is embedded in the queue record and survives a crash.
 * ---------------------------------------------------------------------- */

static int process_job(const char *name, size_t len, unsigned int delivery)
{
    printf("  processing: %.*s  (delivery #%u)\n", (int)len, name, delivery + 1u);

    if (strstr(name, "flaky") != NULL && delivery == 0) {
        printf("  -> transient failure — will retry\n");
        return -1; /* failure */
    }

    printf("  -> success\n");
    return 0; /* success */
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
    const char *db_path = "jobs.qdb";
    qdb_t *db;
    int    rc;

    /* ── 1. Open the queue database ──────────────────────────────────── */

    db = qdb_open(db_path);
    if (!db) {
        fprintf(stderr, "error: qdb_open(\"%s\") failed\n", db_path);
        return EXIT_FAILURE;
    }

    printf("Opened database: %s\n\n", db_path);

    /* ── 2. Push sample jobs ─────────────────────────────────────────── */

    const char *jobs[] = {
        "resize-image-001.jpg",
        "send-welcome-email-42",
        "generate-report-q4",
        "flaky-upload-to-s3-7",   /* will fail once and be NACKed */
        "index-document-99",
    };
    int njobs = (int)(sizeof(jobs) / sizeof(jobs[0]));

    printf("Pushing %d jobs onto queue \"image-jobs\"...\n", njobs);
    for (int i = 0; i < njobs; i++) {
        size_t len = strlen(jobs[i]);
        rc = qdb_push(db, "image-jobs", jobs[i], len);
        if (rc != QDB_OK) {
            fprintf(stderr, "  qdb_push failed: %s\n", qdb_errmsg(rc));
            qdb_close(db);
            return EXIT_FAILURE;
        }
        printf("  pushed: %s\n", jobs[i]);
    }
    printf("\n");

    /* ── 3. Worker loop ──────────────────────────────────────────────── */
    /*
     * In a long-running process, call qdb_process_expired_leases() at the
     * top of every iteration (or on a timer) to reclaim messages whose
     * consumers crashed without ACK/NACKing.
     *
     * We cap at 2 * njobs iterations to guarantee termination in this demo.
     */

    printf("Starting worker loop...\n");

    int processed = 0;
    int max_iterations = njobs * 2;

    for (int iter = 0; iter < max_iterations; iter++) {
        /* Reclaim any expired leases before popping. */
        int expired = qdb_process_expired_leases(db);
        if (expired > 0) {
            printf("  [expire] reclaimed %d lease(s)\n", expired);
        }

        qdb_msg_t msg = {0};
        rc = qdb_pop(db, "image-jobs", &msg);

        if (rc == QDB_ERR_EMPTY) {
            printf("\nQueue empty — all jobs finished.\n");
            break;
        }
        if (rc != QDB_OK) {
            fprintf(stderr, "qdb_pop failed: %s\n", qdb_errmsg(rc));
            qdb_close(db);
            return EXIT_FAILURE;
        }

        printf("[job %llu]\n", (unsigned long long)msg.id);

        /*
         * msg.data is a heap-allocated copy of the raw bytes pushed with
         * qdb_push().  It is NOT null-terminated unless the producer wrote
         * a NUL byte.  Use msg.len for the length.
         *
         * msg.queue is a heap-allocated, null-terminated copy of the queue
         * name.  Both survive any subsequent QDB call on this handle.
         */
        int ok = process_job((const char *)msg.data, msg.len,
                             /* delivery count from msg metadata */ 0u);

        if (ok == 0) {
            rc = qdb_ack(db, msg.id, msg.lease_id);
            if (rc != QDB_OK) {
                fprintf(stderr, "  qdb_ack failed: %s\n", qdb_errmsg(rc));
            }
            processed++;
        } else {
            /*
             * Processing failed transiently.  NACK returns the message to
             * the tail of the queue so it will be retried.  The internal
             * retry_count field is incremented on each NACK or expiry, so
             * callers can implement a dead-letter policy later.
             */
            rc = qdb_nack(db, msg.id, msg.lease_id);
            if (rc != QDB_OK) {
                fprintf(stderr, "  qdb_nack failed: %s\n", qdb_errmsg(rc));
            }
        }

        qdb_msg_free(&msg);
    }

    printf("\nSummary: %d/%d jobs ACKed.\n", processed, njobs);

    /* ── 4. Close ────────────────────────────────────────────────────── */

    qdb_close(db);

    /* Clean up the demo files so re-running starts fresh. */
    remove(db_path);
    remove("jobs.qdb-wal");
    remove("jobs.qdb-lock");

    return EXIT_SUCCESS;
}
