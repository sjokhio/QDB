/*
 * fuzz_record_parser.c — fuzz the record scanner and log replayer
 *
 * The fuzzer supplies raw bytes that are used as the log region of a QDB
 * database file.  The harness prepends a valid 4 096-byte header so the
 * fuzzer can focus mutations on record-level structures.
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

/*
 * Limit input to 128 KiB.  Larger inputs are not interesting for record
 * parsing and would slow each iteration significantly.
 */
#define MAX_LOG_SIZE (128u * 1024u)

/* File magic — must match the definition in qdb_io.c. */
static const uint8_t k_file_magic[8] = {
    0x51u, 0x44u, 0x42u, 0x0Du, 0x0Au, 0x1Au, 0x0Au, 0x00u
};

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
                   "/tmp/qdb_fuzz_rp_%d.qdb",      (int)getpid());
    (void)snprintf(s_lock_path, sizeof(s_lock_path),
                   "/tmp/qdb_fuzz_rp_%d.qdb-lock",  (int)getpid());
    (void)atexit(cleanup_temps);
}

/* -------------------------------------------------------------------------
 * Build a valid header and write header + log_data to s_db_path
 *
 * Having a valid header lets the fuzzer focus on record mutations rather
 * than spending iterations on header-level corner cases (fuzz_header.c
 * covers those independently).
 * ---------------------------------------------------------------------- */

static int write_db_file(const uint8_t *log_data, size_t log_size)
{
    uint8_t  hdr[QDB_HDR_SIZE];
    uint32_t crc;
    FILE    *f;

    memset(hdr, 0, sizeof(hdr));

    memcpy(hdr + QDB_HDR_OFF_MAGIC, k_file_magic, 8);
    qdb__put_u32le(hdr + QDB_HDR_OFF_VERSION,     1u);
    qdb__put_u32le(hdr + QDB_HDR_OFF_PAGE_SIZE,   QDB_PAGE_SIZE);
    qdb__put_u64le(hdr + QDB_HDR_OFF_CREATE_TIME, UINT64_C(1000000));
    qdb__put_u64le(hdr + QDB_HDR_OFF_NEXT_MSG_ID, UINT64_C(1));
    qdb__put_u64le(hdr + QDB_HDR_OFF_LOG_START,   (uint64_t)QDB_HDR_SIZE);
    qdb__put_u64le(hdr + QDB_HDR_OFF_LOG_END,
                   (uint64_t)QDB_HDR_SIZE + (uint64_t)log_size);
    qdb__put_u32le(hdr + QDB_HDR_OFF_FLAGS,       0u);

    crc = qdb__crc32(hdr, QDB_HDR_CRC_COVER);
    qdb__put_u32le(hdr + QDB_HDR_OFF_CRC32, crc);

    f = fopen(s_db_path, "wb");
    if (!f) return -1;

    if (fwrite(hdr, 1, QDB_HDR_SIZE, f) != QDB_HDR_SIZE) {
        (void)fclose(f);
        return -1;
    }
    if (log_size > 0 &&
        fwrite(log_data, 1, log_size, f) != log_size) {
        (void)fclose(f);
        return -1;
    }
    (void)fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------
 * Core fuzz function — called once per input
 * ---------------------------------------------------------------------- */

static int fuzz_one(const uint8_t *data, size_t size)
{
    qdb_t *db;

    ensure_paths();

    if (size > MAX_LOG_SIZE) {
        size = MAX_LOG_SIZE;
    }

    /*
     * Remove any stale lock file from the previous iteration.  This is a
     * safety measure: qdb_close() releases the lock before returning, so in
     * the normal flow the file is unlocked already.  Removing it is harmless
     * and prevents an unexpected QDB_ERR_LOCKED if a prior iteration's
     * qdb_open path left an fd open (which would be a bug in QDB itself).
     */
    (void)remove(s_lock_path);

    if (write_db_file(data, size) != 0) {
        return 0;
    }

    db = qdb_open(s_db_path);
    if (db != NULL) {
        /*
         * Exercise a pop on an arbitrary queue name.  This forces the engine
         * to look up queue state built during replay, exercising the full
         * PENDING-list path as well as the empty-queue return.
         */
        qdb_msg_t msg = {0};
        (void)qdb_pop(db, "fuzz", &msg);
        qdb_msg_free(&msg);
        qdb_close(db);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Entry points
 * ---------------------------------------------------------------------- */

#ifdef QDB_FUZZ_STANDALONE

static uint8_t s_buf[MAX_LOG_SIZE];

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
