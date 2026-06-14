/*
 * hello.c — minimal QDB usage example
 *
 * Demonstrates the complete push / pop / ack cycle, empty-queue handling,
 * and the stats API in about 50 lines of code.
 *
 * Build:  cmake --build build   (included automatically with examples)
 * Run:    ./build/examples/example_hello
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char *path = "hello.qdb";
    qdb_t      *db;
    qdb_msg_t   msg;
    int         rc;

    printf("QDB %s — hello example\n\n", qdb_version());

    /* ── Open (or create) the database ──────────────────────────────────── */

    db = qdb_open(path);
    if (!db) {
        fprintf(stderr, "qdb_open failed\n");
        return EXIT_FAILURE;
    }

    /* ── Push two messages ───────────────────────────────────────────────── */

    rc = qdb_push(db, "greetings", "hello", 5);
    if (rc != QDB_OK) {
        fprintf(stderr, "qdb_push: %s\n", qdb_errmsg(rc));
        qdb_close(db);
        return EXIT_FAILURE;
    }

    rc = qdb_push(db, "greetings", "world", 5);
    if (rc != QDB_OK) {
        fprintf(stderr, "qdb_push: %s\n", qdb_errmsg(rc));
        qdb_close(db);
        return EXIT_FAILURE;
    }

    /* ── Stats before popping ────────────────────────────────────────────── */

    qdb_stats_t st = {0};
    qdb_stats(db, &st);
    printf("After push:  %llu pending, %llu leased, file %llu bytes\n\n",
           (unsigned long long)st.pending_count,
           (unsigned long long)st.leased_count,
           (unsigned long long)st.file_size_bytes);

    /* ── Pop and process each message ────────────────────────────────────── */

    /*
     * Call qdb_process_expired_leases() at the top of every worker loop
     * iteration so that messages whose consumers crashed are redelivered.
     */
    qdb_process_expired_leases(db);

    while (1) {
        msg = (qdb_msg_t){0};
        rc  = qdb_pop(db, "greetings", &msg);

        if (rc == QDB_ERR_EMPTY) {
            printf("Queue empty.\n");
            break;
        }
        if (rc != QDB_OK) {
            fprintf(stderr, "qdb_pop: %s\n", qdb_errmsg(rc));
            break;
        }

        printf("got [id=%llu]: %.*s\n",
               (unsigned long long)msg.id,
               (int)msg.len,
               (const char *)msg.data);

        /* Acknowledge: message is permanently removed from the queue. */
        rc = qdb_ack(db, msg.id, msg.lease_id);
        if (rc != QDB_OK) {
            fprintf(stderr, "qdb_ack: %s\n", qdb_errmsg(rc));
        }

        qdb_msg_free(&msg);
    }

    /* ── Close and clean up ──────────────────────────────────────────────── */

    qdb_close(db);

    /* Remove demo files so re-running starts fresh. */
    remove(path);
    remove("hello.qdb-lock");

    return EXIT_SUCCESS;
}
