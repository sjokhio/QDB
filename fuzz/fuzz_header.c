/*
 * fuzz_header.c — fuzz the file header parser in isolation
 *
 * The fuzzer supplies up to 4 096 bytes that replace the file header region.
 * The log region is left empty (log_start == log_end == QDB_HDR_SIZE) so
 * qdb_open() reaches qdb__header_read() before any record parsing occurs.
 * Any bytes shorter than 4 096 are zero-padded on the right.
 *
 * This gives the fuzzer a narrow, fast target to find:
 *   - magic-byte handling bugs
 *   - CRC check bugs
 *   - Version / page-size boundary bugs
 *   - Offset sanity check bypasses (log_start, log_end, uint64 extremes)
 *   - Header field interactions that yield unexpected internal state
 *
 * Entry point: LLVMFuzzerTestOneInput (libFuzzer / AFL++ libfuzzer mode)
 * Standalone:  compile with -DQDB_FUZZ_STANDALONE; reads files from argv.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb_internal.h"
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
                   "/tmp/qdb_fuzz_hd_%d.qdb",      (int)getpid());
    (void)snprintf(s_lock_path, sizeof(s_lock_path),
                   "/tmp/qdb_fuzz_hd_%d.qdb-lock",  (int)getpid());
    (void)atexit(cleanup_temps);
}

/* -------------------------------------------------------------------------
 * Core fuzz function
 * ---------------------------------------------------------------------- */

static int fuzz_one(const uint8_t *data, size_t size)
{
    uint8_t hdr[QDB_HDR_SIZE];
    qdb_t  *db;
    FILE   *f;
    size_t  copy;

    ensure_paths();

    /*
     * Fill the header with zero, then overlay the fuzz bytes.  Any byte
     * position the fuzzer does not cover stays zero — giving a deterministic
     * baseline rather than whatever happened to be in memory.
     */
    memset(hdr, 0, sizeof(hdr));
    copy = (size < QDB_HDR_SIZE) ? size : QDB_HDR_SIZE;
    memcpy(hdr, data, copy);

    (void)remove(s_lock_path);

    /*
     * Write exactly QDB_HDR_SIZE bytes.  The file has no log region, so if
     * the header's log_end field points beyond the file, qdb_open() must
     * handle the resulting short read gracefully (with QDB_ERR_IO or
     * QDB_ERR_CORRUPT — not a crash).
     */
    f = fopen(s_db_path, "wb");
    if (!f) return 0;

    if (fwrite(hdr, 1, QDB_HDR_SIZE, f) != QDB_HDR_SIZE) {
        (void)fclose(f);
        return 0;
    }
    (void)fclose(f);

    db = qdb_open(s_db_path);
    if (db != NULL) {
        qdb_close(db);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Entry points
 * ---------------------------------------------------------------------- */

#ifdef QDB_FUZZ_STANDALONE

static uint8_t s_buf[QDB_HDR_SIZE];

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
