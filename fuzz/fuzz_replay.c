/*
 * fuzz_replay.c — fuzz qdb_open() with a completely arbitrary file
 *
 * The fuzzer supplies bytes that are written directly to a temp file and
 * then opened with qdb_open().  Unlike fuzz_record_parser.c, the header is
 * also fuzz-controlled, so this target covers the full input space:
 * invalid magic, wrong CRC, impossible offsets, corrupt records, and any
 * interaction between header fields and log content.
 *
 * Entry point: LLVMFuzzerTestOneInput (libFuzzer / AFL++ libfuzzer mode)
 * Standalone:  compile with -DQDB_FUZZ_STANDALONE; reads files from argv.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <unistd.h>
#else
#  include <process.h>
#  define getpid _getpid
#endif

/*
 * Cap at 256 KiB.  The header alone is 4 096 bytes; this leaves 252 KiB
 * for log data — enough to exercise all record types and multi-message
 * sequences while keeping each iteration fast.
 */
#define MAX_FILE_SIZE (256u * 1024u)

/* -------------------------------------------------------------------------
 * Per-process temp-file paths
 * ---------------------------------------------------------------------- */

static char s_db_path[256];
static char s_lock_path[256];

static void cleanup_temps(void)
{
    if (s_db_path[0])   (void)remove(s_db_path);
    if (s_lock_path[0]) (void)remove(s_lock_path);
}

static void ensure_paths(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    (void)snprintf(s_db_path,   sizeof(s_db_path),
                   "/tmp/qdb_fuzz_re_%d.qdb",      (int)getpid());
    (void)snprintf(s_lock_path, sizeof(s_lock_path),
                   "/tmp/qdb_fuzz_re_%d.qdb-lock",  (int)getpid());
    (void)atexit(cleanup_temps);
}

/* -------------------------------------------------------------------------
 * Core fuzz function
 * ---------------------------------------------------------------------- */

static int fuzz_one(const uint8_t *data, size_t size)
{
    qdb_t *db;
    FILE  *f;

    ensure_paths();

    if (size > MAX_FILE_SIZE) {
        size = MAX_FILE_SIZE;
    }

    (void)remove(s_lock_path);

    f = fopen(s_db_path, "wb");
    if (!f) return 0;

    if (size > 0 && fwrite(data, 1, size, f) != size) {
        (void)fclose(f);
        return 0;
    }
    (void)fclose(f);

    db = qdb_open(s_db_path);
    if (db != NULL) {
        /*
         * If the file happened to be a valid database, push a message and
         * exercise the pop path.  This verifies that recovered in-memory
         * state is usable, not just syntactically correct.
         */
        const char payload[] = "fuzz";
        if (qdb_push(db, "fuzz", payload, sizeof(payload) - 1u) == QDB_OK) {
            qdb_msg_t msg = {0};
            if (qdb_pop(db, "fuzz", &msg) == QDB_OK) {
                (void)qdb_ack(db, msg.id, msg.lease_id);
                qdb_msg_free(&msg);
            }
        }
        qdb_close(db);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Entry points
 * ---------------------------------------------------------------------- */

#ifdef QDB_FUZZ_STANDALONE

static uint8_t s_buf[MAX_FILE_SIZE];

int main(int argc, char **argv)
{
    int i;
    if (argc < 2) {
        (void)fprintf(stderr, "usage: %s <corpus-file> [...]\n", argv[0]);
        return 1;
    }
    for (i = 1; i < argc; i++) {
        FILE   *f;
        size_t  n;

        f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); continue; }
        n = fread(s_buf, 1, sizeof(s_buf), f);
        (void)fclose(f);

        (void)printf("[%s] %zu bytes\n", argv[i], n);
        fuzz_one(s_buf, n);
    }
    return 0;
}

#else  /* libFuzzer / AFL++ libfuzzer mode */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return fuzz_one(data, size);
}

#endif /* QDB_FUZZ_STANDALONE */
