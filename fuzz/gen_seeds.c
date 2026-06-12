/*
 * gen_seeds.c — generate initial fuzz corpus files from valid QDB databases
 *
 * This tool creates a set of minimal, valid QDB database files covering
 * every record type.  These seed files let the fuzzer start from meaningful
 * inputs rather than random bytes, dramatically improving coverage speed.
 *
 * Usage:
 *   ./build/fuzz/gen_seeds [output-dir]
 *   output-dir defaults to "fuzz/corpus" relative to the current directory.
 *
 * The tool creates three sub-directories and writes seeds into each:
 *   corpus/record_parser/   — log regions for fuzz_record_parser
 *   corpus/replay/          — complete database files for fuzz_replay
 *   corpus/header/          — 4 096-byte headers for fuzz_header
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static int mkdirp(const char *path)
{
#if defined(_WIN32)
    (void)path;
    return 0; /* skip on Windows — user creates dirs manually */
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
#endif
}

static void make_dirs(const char *base)
{
    char buf[512];
    (void)mkdirp(base);
    (void)snprintf(buf, sizeof(buf), "%s/record_parser", base);
    (void)mkdirp(buf);
    (void)snprintf(buf, sizeof(buf), "%s/replay", base);
    (void)mkdirp(buf);
    (void)snprintf(buf, sizeof(buf), "%s/header", base);
    (void)mkdirp(buf);
}

/* Write len bytes of a file to an output path. */
static int copy_file(const char *src, const char *dst, size_t skip, size_t len)
{
    FILE  *in  = fopen(src, "rb");
    FILE  *out;
    uint8_t buf[4096];
    size_t remaining = len;
    size_t n;

    if (!in) { perror(src); return -1; }

    if (fseek(in, (long)skip, SEEK_SET) != 0) {
        (void)fclose(in);
        return -1;
    }

    out = fopen(dst, "wb");
    if (!out) {
        (void)fclose(in);
        perror(dst);
        return -1;
    }

    while (remaining > 0) {
        size_t want = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        n = fread(buf, 1, want, in);
        if (n == 0) break;
        if (fwrite(buf, 1, n, out) != n) {
            (void)fclose(in);
            (void)fclose(out);
            return -1;
        }
        remaining -= n;
    }

    (void)fclose(in);
    (void)fclose(out);
    return 0;
}

static long file_size_of(const char *path)
{
    FILE *f = fopen(path, "rb");
    long  sz;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { (void)fclose(f); return -1; }
    sz = ftell(f);
    (void)fclose(f);
    return sz;
}

static void remove_files(const char *base)
{
    char buf[512];
    (void)remove(base);
    (void)snprintf(buf, sizeof(buf), "%s-wal",  base); (void)remove(buf);
    (void)snprintf(buf, sizeof(buf), "%s-lock", base); (void)remove(buf);
}

/* -------------------------------------------------------------------------
 * Seed scenarios
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *name;        /* seed base name, e.g. "push" */
    const char *description; /* human-readable label         */
} seed_t;

static const seed_t k_seeds[] = {
    { "empty",      "empty database (header only)"         },
    { "push",       "single message pushed"                },
    { "push_ack",   "push → pop → ack"                    },
    { "push_nack",  "push → pop → nack (message requeued)" },
    { "push_lease", "push → pop (message still leased)"    },
    { "multi",      "three queues, five messages"           },
};

static const int k_nseed = (int)(sizeof(k_seeds) / sizeof(k_seeds[0]));

/* Build a specific seed database at `db_path`.  Returns 0 on success. */
static int build_seed(const char *name, const char *db_path)
{
    qdb_t     *db;
    qdb_msg_t  msg = {0};
    int        rc  = 0;

    db = qdb_open(db_path);
    if (!db) {
        (void)fprintf(stderr, "error: qdb_open(%s) failed\n", db_path);
        return -1;
    }

    if (strcmp(name, "empty") == 0) {
        /* nothing to push */

    } else if (strcmp(name, "push") == 0) {
        rc = qdb_push(db, "jobs", "hello world", 11);

    } else if (strcmp(name, "push_ack") == 0) {
        rc = qdb_push(db, "jobs", "hello world", 11);
        if (rc == QDB_OK) rc = qdb_pop(db, "jobs", &msg);
        if (rc == QDB_OK) rc = qdb_ack(db, msg.id, msg.lease_id);
        qdb_msg_free(&msg);

    } else if (strcmp(name, "push_nack") == 0) {
        rc = qdb_push(db, "jobs", "hello world", 11);
        if (rc == QDB_OK) rc = qdb_pop(db, "jobs", &msg);
        if (rc == QDB_OK) rc = qdb_nack(db, msg.id, msg.lease_id);
        qdb_msg_free(&msg);

    } else if (strcmp(name, "push_lease") == 0) {
        rc = qdb_push(db, "jobs", "hello world", 11);
        if (rc == QDB_OK) rc = qdb_pop(db, "jobs", &msg);
        /* intentionally leave leased — do not ack/nack */
        qdb_msg_free(&msg);

    } else if (strcmp(name, "multi") == 0) {
        rc = qdb_push(db, "alpha", "msg-a1", 6);
        if (rc == QDB_OK) rc = qdb_push(db, "alpha", "msg-a2", 6);
        if (rc == QDB_OK) rc = qdb_push(db, "beta",  "msg-b1", 6);
        if (rc == QDB_OK) rc = qdb_push(db, "beta",  "msg-b2", 6);
        if (rc == QDB_OK) rc = qdb_push(db, "gamma", "msg-g1", 6);
        /* pop one from alpha and ack it */
        if (rc == QDB_OK) rc = qdb_pop(db, "alpha", &msg);
        if (rc == QDB_OK) rc = qdb_ack(db, msg.id, msg.lease_id);
        qdb_msg_free(&msg);
        /* pop one from beta and nack it */
        if (rc == QDB_OK) rc = qdb_pop(db, "beta", &msg);
        if (rc == QDB_OK) rc = qdb_nack(db, msg.id, msg.lease_id);
        qdb_msg_free(&msg);
    }

    if (rc != QDB_OK) {
        (void)fprintf(stderr, "warning: seed '%s' API call failed (%d)\n",
                      name, rc);
    }

    qdb_close(db);
    return 0;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *outdir = (argc > 1) ? argv[1] : "fuzz/corpus";
    char        db_path[512];
    char        dst_replay[512];
    char        dst_record[512];
    char        dst_header[512];
    int         i;

    (void)printf("Generating seed corpus in: %s\n", outdir);
    make_dirs(outdir);

    for (i = 0; i < k_nseed; i++) {
        const char *name = k_seeds[i].name;
        long        fsz;
        long        log_sz;

        (void)snprintf(db_path, sizeof(db_path),
                       "/tmp/qdb_seed_%s.qdb", name);
        remove_files(db_path);

        if (build_seed(name, db_path) != 0) {
            continue;
        }

        fsz = file_size_of(db_path);
        if (fsz < 4096) {
            (void)fprintf(stderr, "warning: seed '%s' too small (%ld)\n",
                          name, fsz);
            remove_files(db_path);
            continue;
        }

        /* ── replay corpus: full file ──────────────────────────────── */
        (void)snprintf(dst_replay, sizeof(dst_replay),
                       "%s/replay/%s.qdb", outdir, name);
        if (copy_file(db_path, dst_replay, 0, (size_t)fsz) == 0) {
            (void)printf("  replay/%-20s  %ld bytes\n", name, fsz);
        }

        /* ── header corpus: first 4096 bytes ────────────────────────── */
        (void)snprintf(dst_header, sizeof(dst_header),
                       "%s/header/%s.hdr", outdir, name);
        if (copy_file(db_path, dst_header, 0, 4096u) == 0) {
            (void)printf("  header/%-20s  4096 bytes\n", name);
        }

        /* ── record_parser corpus: log region only ───────────────────── */
        log_sz = fsz - 4096;
        if (log_sz > 0) {
            (void)snprintf(dst_record, sizeof(dst_record),
                           "%s/record_parser/%s.log", outdir, name);
            if (copy_file(db_path, dst_record, 4096u, (size_t)log_sz) == 0) {
                (void)printf("  record_parser/%-15s  %ld bytes\n",
                             name, log_sz);
            }
        }

        remove_files(db_path);
        (void)printf("  [%s] %s\n\n", name, k_seeds[i].description);
    }

    (void)printf("Done.  Seeds written to %s/\n", outdir);
    return 0;
}
