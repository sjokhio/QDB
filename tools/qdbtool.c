/*
 * qdbtool.c — QDB command-line inspection and maintenance utility
 *
 * Provides commands to inspect, monitor, and maintain QDB database files.
 * All operations are performed through the public QDB API only.
 * No internal headers, no external dependencies beyond libc.
 *
 * Usage:
 *   qdbtool <command> [flags] [--] <path>
 *
 * Commands:
 *   info    [--json] <path>            Database summary
 *   list    [--json] <path>            List queue names
 *   stats   [--json] <path> [<queue>]  Per-queue statistics
 *   compact [--force] [--dry-run] <path>  Reclaim disk space
 *   verify  [--json] <path>            Validate an existing database file
 *
 * All commands require the database file to already exist.  None of them
 * create a new database.  Use -- before <path> if the path begins with -.
 *
 * verify performs normal open-time validation (header check, CRC check,
 * log replay, tail truncation).  It does not perform additional offline
 * scanning beyond what qdb_open() already does.
 *
 * Note: qdbtool acquires the same exclusive file lock as qdb_open().
 * A database open by another process cannot be accessed; stop the
 * application before running qdbtool.
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
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qdb.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/*
 * file_exists — return 1 if path refers to an existing regular file.
 *
 * Uses fopen("rb") for portability across all three supported platforms.
 * A TOCTOU race between this check and qdb_open() is acceptable in a
 * single-user CLI tool.
 */
static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { return 0; }
    fclose(f);
    return 1;
}

/*
 * json_str — write a JSON-escaped string literal (with surrounding quotes)
 * to stdout.  Handles all mandatory RFC 8259 §7 escape sequences.
 *
 * Bytes in the range 0x80–0xFF are passed through as-is; the caller is
 * assumed to supply valid UTF-8, which is what JSON requires.  Invalid
 * UTF-8 byte sequences produce invalid JSON — the same byte sequence that
 * was stored in the queue name.
 */
static void json_str(const char *s)
{
    unsigned char c;
    putchar('"');
    for (; *s; s++) {
        c = (unsigned char)*s;
        if      (c == '"')  { fputs("\\\"", stdout); }
        else if (c == '\\') { fputs("\\\\", stdout); }
        else if (c == '\n') { fputs("\\n",  stdout); }
        else if (c == '\r') { fputs("\\r",  stdout); }
        else if (c == '\t') { fputs("\\t",  stdout); }
        else if (c < 0x20)  { printf("\\u%04x", (unsigned)c); }
        else                { putchar((int)c); }
    }
    putchar('"');
}

/*
 * fmt_bytes — format a byte count as a human-readable string.
 * Writes into buf (capacity sz) and returns buf.
 */
static const char *fmt_bytes(uint64_t n, char *buf, size_t sz)
{
    if (n >= (uint64_t)1024 * 1024 * 1024)
        snprintf(buf, sz, "%.2f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
    else if (n >= (uint64_t)1024 * 1024)
        snprintf(buf, sz, "%.2f MB", (double)n / (1024.0 * 1024.0));
    else if (n >= 1024u)
        snprintf(buf, sz, "%.2f KB", (double)n / 1024.0);
    else
        snprintf(buf, sz, "%llu B", (unsigned long long)n);
    return buf;
}

/*
 * open_db — check that path exists, then open the database.
 *
 * Inspection commands must not create a new database; the existence check
 * ensures qdb_open() is only called when the file is already present.
 * Returns NULL on any failure after printing a diagnostic to stderr.
 */
static qdb_t *open_db(const char *path)
{
    qdb_t *db;

    if (!file_exists(path)) {
        fprintf(stderr, "qdbtool: %s: file not found\n", path);
        return NULL;
    }

    db = qdb_open(path);
    if (!db) {
        fprintf(stderr, "qdbtool: %s: could not open database\n", path);
        fprintf(stderr,
                "note: qdbtool requires exclusive access; "
                "if another process has this database open, stop it first\n");
    }
    return db;
}

/* -------------------------------------------------------------------------
 * info command
 *
 * Prints a database-level summary: file path, size, queue count, message
 * state counts, and whether compaction is recommended.
 * ---------------------------------------------------------------------- */

static int cmd_info(const char *path, int json)
{
    qdb_t      *db;
    qdb_stats_t st  = {0};
    int         rec = 0;
    int         rc;
    char        sizebuf[32];

    db = open_db(path);
    if (!db) { return 1; }

    rc = qdb_stats(db, &st);
    if (rc != QDB_OK) {
        fprintf(stderr, "qdbtool: %s: stats failed: %s\n", path, qdb_errmsg(rc));
        qdb_close(db);
        return 1;
    }

    rc = qdb_compact_recommended(db, &rec);
    if (rc != QDB_OK) {
        fprintf(stderr, "qdbtool: %s: compact check failed: %s\n", path, qdb_errmsg(rc));
        qdb_close(db);
        return 1;
    }

    qdb_close(db);

    if (json) {
        printf("{\"file\":");
        json_str(path);
        printf(",\"size_bytes\":%llu",   (unsigned long long)st.file_size_bytes);
        printf(",\"queues\":%llu",        (unsigned long long)st.queue_count);
        printf(",\"pending\":%llu",       (unsigned long long)st.pending_count);
        printf(",\"leased\":%llu",        (unsigned long long)st.leased_count);
        printf(",\"acked\":%llu",         (unsigned long long)st.acked_count);
        printf(",\"compact_advised\":%s", rec ? "true" : "false");
        printf("}\n");
    } else {
        printf("File:            %s\n", path);
        printf("Size:            %s\n",
               fmt_bytes(st.file_size_bytes, sizebuf, sizeof(sizebuf)));
        printf("Queues:          %llu\n", (unsigned long long)st.queue_count);
        printf("Pending:         %llu\n", (unsigned long long)st.pending_count);
        printf("Leased:          %llu\n", (unsigned long long)st.leased_count);
        printf("Acked:           %llu\n", (unsigned long long)st.acked_count);
        printf("Compact advised: %s\n",   rec ? "yes" : "no");
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * list command
 *
 * Prints the names of all queues in the database, one per line.
 * In JSON mode, prints a JSON string array.
 * ---------------------------------------------------------------------- */

static int cmd_list(const char *path, int json)
{
    qdb_t            *db;
    qdb_queue_name_t *names = NULL;
    size_t            count = 0;
    size_t            i;
    int               rc;

    db = open_db(path);
    if (!db) { return 1; }

    rc = qdb_queue_list(db, NULL, 0, &count);
    if (rc != QDB_OK) {
        fprintf(stderr, "qdbtool: %s: queue list failed: %s\n", path, qdb_errmsg(rc));
        qdb_close(db);
        return 1;
    }

    if (count > 0) {
        names = (qdb_queue_name_t *)malloc(count * sizeof(*names));
        if (!names) {
            fprintf(stderr, "qdbtool: out of memory\n");
            qdb_close(db);
            return 1;
        }
        rc = qdb_queue_list(db, names, count, &count);
        if (rc != QDB_OK) {
            fprintf(stderr, "qdbtool: %s: queue list failed: %s\n", path, qdb_errmsg(rc));
            free(names);
            qdb_close(db);
            return 1;
        }
    }

    qdb_close(db);

    if (json) {
        putchar('[');
        for (i = 0; i < count; i++) {
            if (i > 0) putchar(',');
            json_str(names[i].name);
        }
        puts("]");
    } else {
        for (i = 0; i < count; i++) {
            puts(names[i].name);
        }
    }

    free(names);
    return 0;
}

/* -------------------------------------------------------------------------
 * stats command
 *
 * With a queue name: prints stats for that queue only.
 * Without a queue name: lists all queues with their stats in a table.
 * ---------------------------------------------------------------------- */

static int cmd_stats(const char *path, const char *queue, int json)
{
    qdb_t             *db;
    qdb_queue_stats_t  qs = {0};
    int                rc;

    db = open_db(path);
    if (!db) { return 1; }

    if (queue) {
        rc = qdb_queue_stats(db, queue, &qs);
        qdb_close(db);

        if (rc != QDB_OK) {
            fprintf(stderr, "qdbtool: %s: %s\n", queue, qdb_errmsg(rc));
            return 1;
        }

        if (json) {
            printf("{\"queue\":");
            json_str(queue);
            printf(",\"pending\":%llu,\"leased\":%llu,\"acked\":%llu}\n",
                   (unsigned long long)qs.pending_count,
                   (unsigned long long)qs.leased_count,
                   (unsigned long long)qs.acked_count);
        } else {
            printf("Queue:    %s\n",   queue);
            printf("Pending:  %llu\n", (unsigned long long)qs.pending_count);
            printf("Leased:   %llu\n", (unsigned long long)qs.leased_count);
            printf("Acked:    %llu\n", (unsigned long long)qs.acked_count);
        }
        return 0;
    }

    /* All-queues mode: collect names and stats while db is open. */
    {
        qdb_queue_name_t  *names  = NULL;
        qdb_queue_stats_t *all_qs = NULL;
        size_t             count  = 0;
        size_t             i;

        rc = qdb_queue_list(db, NULL, 0, &count);
        if (rc != QDB_OK) {
            fprintf(stderr, "qdbtool: %s: queue list failed: %s\n", path, qdb_errmsg(rc));
            qdb_close(db);
            return 1;
        }

        if (count > 0) {
            names  = (qdb_queue_name_t  *)malloc(count * sizeof(*names));
            all_qs = (qdb_queue_stats_t *)malloc(count * sizeof(*all_qs));
            if (!names || !all_qs) {
                fprintf(stderr, "qdbtool: out of memory\n");
                free(names);
                free(all_qs);
                qdb_close(db);
                return 1;
            }

            rc = qdb_queue_list(db, names, count, &count);
            if (rc != QDB_OK) {
                fprintf(stderr, "qdbtool: %s: queue list failed: %s\n", path, qdb_errmsg(rc));
                free(names);
                free(all_qs);
                qdb_close(db);
                return 1;
            }

            for (i = 0; i < count; i++) {
                memset(&all_qs[i], 0, sizeof(all_qs[i]));
                rc = qdb_queue_stats(db, names[i].name, &all_qs[i]);
                if (rc != QDB_OK) {
                    fprintf(stderr, "qdbtool: %s: queue stats failed: %s\n",
                            names[i].name, qdb_errmsg(rc));
                    free(names);
                    free(all_qs);
                    qdb_close(db);
                    return 1;
                }
            }
        }

        qdb_close(db);

        if (json) {
            putchar('[');
            for (i = 0; i < count; i++) {
                if (i > 0) putchar(',');
                printf("{\"queue\":");
                json_str(names[i].name);
                printf(",\"pending\":%llu,\"leased\":%llu,\"acked\":%llu}",
                       (unsigned long long)all_qs[i].pending_count,
                       (unsigned long long)all_qs[i].leased_count,
                       (unsigned long long)all_qs[i].acked_count);
            }
            puts("]");
        } else {
            printf("%-24.24s  %9s  %9s  %9s\n",
                   "Queue", "Pending", "Leased", "Acked");
            if (count == 0) {
                printf("(no queues)\n");
            } else {
                for (i = 0; i < count; i++) {
                    printf("%-24.24s  %9llu  %9llu  %9llu\n",
                           names[i].name,
                           (unsigned long long)all_qs[i].pending_count,
                           (unsigned long long)all_qs[i].leased_count,
                           (unsigned long long)all_qs[i].acked_count);
                }
            }
        }

        free(names);
        free(all_qs);
        return 0;
    }
}

/* -------------------------------------------------------------------------
 * compact command
 *
 * Rewrites the database to reclaim space from ACKed records.
 *
 * Flags:
 *   --force    Compact even when qdb_compact_recommended() returns 0.
 *   --dry-run  Print recommendation and size without compacting.
 *
 * Always calls qdb_process_expired_leases() before qdb_compact() so that
 * already-expired leases are not written into the compacted file as active
 * LEASED messages.  If lease expiry fails, the compact is aborted.
 * ---------------------------------------------------------------------- */

static int cmd_compact(const char *path, int force, int dry_run)
{
    qdb_t      *db;
    qdb_stats_t before = {0};
    qdb_stats_t after  = {0};
    int         rec    = 0;
    int         rc;
    char        buf_b[32], buf_a[32], buf_r[32];

    db = open_db(path);
    if (!db) { return 1; }

    rc = qdb_stats(db, &before);
    if (rc != QDB_OK) {
        fprintf(stderr, "qdbtool: %s: stats failed: %s\n", path, qdb_errmsg(rc));
        qdb_close(db);
        return 1;
    }

    rc = qdb_compact_recommended(db, &rec);
    if (rc != QDB_OK) {
        fprintf(stderr, "qdbtool: %s: compact check failed: %s\n", path, qdb_errmsg(rc));
        qdb_close(db);
        return 1;
    }

    if (dry_run) {
        qdb_close(db);
        printf("Compact recommended: %s\n", rec ? "yes" : "no");
        printf("Current size:        %s\n",
               fmt_bytes(before.file_size_bytes, buf_b, sizeof(buf_b)));
        printf("(dry run -- no changes made)\n");
        return 0;
    }

    if (!rec && !force) {
        qdb_close(db);
        printf("Compaction not needed (acked_count not significant).\n");
        printf("Use --force to compact anyway.\n");
        return 0;
    }

    printf("Compacting %s...\n", path);
    fflush(stdout);

    rc = qdb_process_expired_leases(db);
    if (rc < 0) {
        fprintf(stderr, "qdbtool: %s: lease expiry failed: %s\n",
                path, qdb_errmsg(rc));
        qdb_close(db);
        return 1;
    }

    rc = qdb_compact(db);
    if (rc != QDB_OK) {
        fprintf(stderr, "qdbtool: %s: compact failed: %s\n",
                path, qdb_errmsg(rc));
        qdb_close(db);
        return 1;
    }

    rc = qdb_stats(db, &after);
    if (rc != QDB_OK) {
        /* Compact succeeded; post-compact stats are informational only. */
        fprintf(stderr, "qdbtool: %s: post-compact stats failed (compact succeeded): %s\n",
                path, qdb_errmsg(rc));
    }
    qdb_close(db);

    printf("Before:    %s\n", fmt_bytes(before.file_size_bytes, buf_b, sizeof(buf_b)));
    printf("After:     %s\n", fmt_bytes(after.file_size_bytes,  buf_a, sizeof(buf_a)));
    if (before.file_size_bytes > after.file_size_bytes) {
        printf("Reclaimed: %s\n",
               fmt_bytes(before.file_size_bytes - after.file_size_bytes,
                         buf_r, sizeof(buf_r)));
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * verify command
 *
 * Confirms that an existing database file can be opened and fully replayed.
 * The file must already exist; verify does not create a new database.
 *
 * Validation performed by qdb_open():
 *   - File header magic and CRC-32 check
 *   - Record-by-record CRC-32 verification across the full log
 *   - Partial tail record detection and truncation
 *   - Consistency checks: duplicate IDs, wrong-lease operations, etc.
 *
 * verify does not perform any additional offline scanning beyond the normal
 * open-time path.
 * ---------------------------------------------------------------------- */

static int cmd_verify(const char *path, int json)
{
    qdb_t *db = open_db(path);

    if (!db) {
        if (json) {
            printf("{\"ok\":false,\"error\":\"could not open database\"}\n");
        }
        /* open_db already printed the human-readable error to stderr */
        return 1;
    }

    qdb_close(db);

    if (json) {
        printf("{\"ok\":true}\n");
    } else {
        printf("%s: ok\n", path);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <command> [flags] [--] <path>\n"
            "\n"
            "Commands:\n"
            "  info    [--json] <path>           Database summary\n"
            "  list    [--json] <path>           List queue names\n"
            "  stats   [--json] <path> [<queue>] Per-queue statistics\n"
            "  compact [--force] [--dry-run] <path>  Reclaim disk space\n"
            "  verify  [--json] <path>           Validate existing database file\n"
            "\n"
            "Flags:\n"
            "  --json      Machine-readable JSON output (info, list, stats, verify)\n"
            "  --force     Compact even when not recommended\n"
            "  --dry-run   Report recommendation without compacting\n"
            "  --          End of flags; treat next argument as <path>\n"
            "\n"
            "Exit codes: 0 success  1 database/runtime error  2 usage error\n"
            "\n"
            "All commands require the database file to already exist.\n"
            "None create a new database.\n"
            "\n"
            "Note: qdbtool requires exclusive database access.\n"
            "      Stop the application before running qdbtool.\n"
            "\n"
            "verify performs normal open-time validation only (header, CRC,\n"
            "log replay).  It does not perform additional offline scanning.\n",
            prog);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *prog         = argv[0] ? argv[0] : "qdbtool";
    const char *cmd;
    const char *path         = NULL;
    const char *queue        = NULL;
    int         json         = 0;
    int         force        = 0;
    int         dry          = 0;
    int         end_of_flags = 0;
    int         i;

    if (argc < 2) {
        usage(prog);
        return 2;
    }

    cmd = argv[1];

    /*
     * Parse remaining argv: flags and positional arguments.
     * -- ends flag parsing; subsequent tokens are always treated as positional.
     * After --, subsequent tokens are always treated as positional.
     */
    for (i = 2; i < argc; i++) {
        if (!end_of_flags && strcmp(argv[i], "--") == 0) {
            end_of_flags = 1;
            continue;
        }
        if (!end_of_flags && argv[i][0] == '-') {
            if      (strcmp(argv[i], "--json")    == 0) { json  = 1; }
            else if (strcmp(argv[i], "--force")   == 0) { force = 1; }
            else if (strcmp(argv[i], "--dry-run") == 0) { dry   = 1; }
            else {
                fprintf(stderr, "qdbtool: unknown flag: %s\n", argv[i]);
                return 2;
            }
        } else if (!path)  {
            path  = argv[i];
        } else if (!queue) {
            queue = argv[i];
        } else {
            fprintf(stderr, "qdbtool: unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }

    /* Command dispatch with per-command flag and argument validation. */

    if (strcmp(cmd, "help") == 0 ||
        strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        usage(prog);
        return 0;
    }

    if (strcmp(cmd, "info") == 0) {
        if (!path)  { fprintf(stderr, "qdbtool: info: missing <path>\n"); return 2; }
        if (queue)  { fprintf(stderr, "qdbtool: info: unexpected argument: %s\n", queue); return 2; }
        if (force)  { fprintf(stderr, "qdbtool: info: --force is not valid for this command\n"); return 2; }
        if (dry)    { fprintf(stderr, "qdbtool: info: --dry-run is not valid for this command\n"); return 2; }
        return cmd_info(path, json);
    }

    if (strcmp(cmd, "list") == 0) {
        if (!path)  { fprintf(stderr, "qdbtool: list: missing <path>\n"); return 2; }
        if (queue)  { fprintf(stderr, "qdbtool: list: unexpected argument: %s\n", queue); return 2; }
        if (force)  { fprintf(stderr, "qdbtool: list: --force is not valid for this command\n"); return 2; }
        if (dry)    { fprintf(stderr, "qdbtool: list: --dry-run is not valid for this command\n"); return 2; }
        return cmd_list(path, json);
    }

    if (strcmp(cmd, "stats") == 0) {
        if (!path)  { fprintf(stderr, "qdbtool: stats: missing <path>\n"); return 2; }
        if (force)  { fprintf(stderr, "qdbtool: stats: --force is not valid for this command\n"); return 2; }
        if (dry)    { fprintf(stderr, "qdbtool: stats: --dry-run is not valid for this command\n"); return 2; }
        return cmd_stats(path, queue, json);   /* queue is optional */
    }

    if (strcmp(cmd, "compact") == 0) {
        if (!path)  { fprintf(stderr, "qdbtool: compact: missing <path>\n"); return 2; }
        if (queue)  { fprintf(stderr, "qdbtool: compact: unexpected argument: %s\n", queue); return 2; }
        if (json)   { fprintf(stderr, "qdbtool: compact: --json is not supported\n"); return 2; }
        return cmd_compact(path, force, dry);
    }

    if (strcmp(cmd, "verify") == 0) {
        if (!path)  { fprintf(stderr, "qdbtool: verify: missing <path>\n"); return 2; }
        if (queue)  { fprintf(stderr, "qdbtool: verify: unexpected argument: %s\n", queue); return 2; }
        if (force)  { fprintf(stderr, "qdbtool: verify: --force is not valid for this command\n"); return 2; }
        if (dry)    { fprintf(stderr, "qdbtool: verify: --dry-run is not valid for this command\n"); return 2; }
        return cmd_verify(path, json);
    }

    fprintf(stderr, "qdbtool: unknown command: %s\n", cmd);
    usage(prog);
    return 2;
}
