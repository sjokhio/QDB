/*
 * test_storage.c — storage-layer tests for QDB
 *
 * Tests qdb_open / qdb_close and the internal record I/O primitives.
 * Includes qdb_internal.h directly so it can call internal functions.
 *
 * Each test creates files under the build directory; they are removed
 * before and after each test so runs are hermetic.
 *
 * SPDX-License-Identifier: MIT
 */

/* Pull in POSIX pread/pwrite for the raw file helpers used by the tests. */
#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#  if !defined(_FILE_OFFSET_BITS)
#    define _FILE_OFFSET_BITS 64
#  endif
#endif

#include "../src/qdb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
#endif

/* -------------------------------------------------------------------------
 * Minimal test harness
 * ---------------------------------------------------------------------- */

static int g_tests_run    = 0;
static int g_tests_failed = 0;
static int g_test_ok      = 1;   /* reset to 1 at start of each test */

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

#define ASSERT_EQ(a, b)   ASSERT((a) == (b))
#define ASSERT_NE(a, b)   ASSERT((a) != (b))
#define ASSERT_NULL(p)    ASSERT((p) == NULL)
#define ASSERT_NOTNULL(p) ASSERT((p) != NULL)

static void test_begin(const char *name)
{
    g_test_ok = 1;
    printf("  %-55s", name);
    fflush(stdout);
}

static void test_end(void)
{
    printf("%s\n", g_test_ok ? "ok" : "FAILED");
}

/* -------------------------------------------------------------------------
 * Portable raw file helpers used to create test fixtures
 * ---------------------------------------------------------------------- */

/* Write len bytes from buf at offset in the file at path. */
static int raw_write_at(const char *path, uint64_t offset,
                        const void *buf, size_t len)
{
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { return -1; }
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    SetFilePointerEx(h, li, NULL, FILE_BEGIN);
    DWORD written = 0;
    WriteFile(h, buf, (DWORD)len, &written, NULL);
    CloseHandle(h);
    return ((size_t)written == len) ? 0 : -1;
#else
    int fd = open(path, O_WRONLY);
    if (fd < 0) { return -1; }
    ssize_t n = pwrite(fd, buf, len, (off_t)offset);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
#endif
}

/* Return the size of the file at path, or -1 on error. */
static int64_t raw_file_size(const char *path)
{
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { return -1; }
    LARGE_INTEGER li;
    BOOL ok = GetFileSizeEx(h, &li);
    CloseHandle(h);
    return ok ? (int64_t)li.QuadPart : -1;
#else
    struct stat st;
    if (stat(path, &st) != 0) { return -1; }
    return (int64_t)st.st_size;
#endif
}

/* Delete path and its sidecar files (-wal, -lock). */
static void cleanup(const char *path)
{
    char sidecar[512];
    size_t plen = strlen(path);

#if defined(_WIN32)
    DeleteFileA(path);
    if (plen + 5 < sizeof(sidecar)) {
        memcpy(sidecar, path, plen);
        memcpy(sidecar + plen, "-wal",  5);
        DeleteFileA(sidecar);
        memcpy(sidecar + plen, "-lock", 6);
        DeleteFileA(sidecar);
    }
#else
    (void)unlink(path);
    if (plen + 6 < sizeof(sidecar)) {
        memcpy(sidecar, path, plen);
        memcpy(sidecar + plen, "-wal",  5);
        (void)unlink(sidecar);
        memcpy(sidecar + plen, "-lock", 6);
        (void)unlink(sidecar);
    }
#endif
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

static void test_crc32_known_values(void)
{
    test_begin("crc32: empty input produces 0x00000000");
    ASSERT_EQ(qdb__crc32("", 0), 0x00000000u);
    test_end();

    test_begin("crc32: \"123456789\" produces 0xCBF43926");
    ASSERT_EQ(qdb__crc32("123456789", 9), 0xCBF43926u);
    test_end();

    test_begin("crc32: incremental matches one-shot");
    {
        uint32_t inc = qdb__crc32_end(
            qdb__crc32_update(
            qdb__crc32_update(qdb__crc32_begin(), "1234", 4),
                              "56789", 5));
        uint32_t full = qdb__crc32("123456789", 9);
        ASSERT_EQ(inc, full);
    }
    test_end();
}

static void test_open_new_database(void)
{
    const char *path = "qdb_test_new.qdb";

    test_begin("open new database: succeeds");
    cleanup(path);
    qdb_t *db = qdb_open(path);
    ASSERT_NOTNULL(db);
    test_end();

    test_begin("open new database: log offsets are QDB_HDR_SIZE");
    if (db) {
        ASSERT_EQ(db->log_start_offset, (uint64_t)QDB_HDR_SIZE);
        ASSERT_EQ(db->log_end_offset,   (uint64_t)QDB_HDR_SIZE);
    }
    test_end();

    test_begin("open new database: next_msg_id starts at 1");
    if (db) {
        ASSERT_EQ(db->next_msg_id, 1u);
    }
    test_end();

    test_begin("open new database: file size equals QDB_HDR_SIZE");
    if (db) {
        int64_t sz = raw_file_size(path);
        ASSERT_EQ(sz, (int64_t)QDB_HDR_SIZE);
    }
    test_end();

    if (db) { qdb_close(db); }
    cleanup(path);
}

static void test_reopen_existing_database(void)
{
    const char *path = "qdb_test_reopen.qdb";

    test_begin("reopen existing database: succeeds");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) { qdb_close(db); }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) { qdb_close(db); }
    }
    test_end();

    cleanup(path);
}

static void test_invalid_header_magic(void)
{
    const char *path = "qdb_test_badmagic.qdb";
    static const uint8_t garbage[QDB_HDR_SIZE] = { 0 };

    test_begin("corrupt magic: qdb_open returns NULL");
    cleanup(path);

    /* Write a file that is the right size but has wrong magic. */
    {
        qdb__fd_t fd = QDB__INVALID_FD;
        int is_new   = 0;
        if (qdb__file_open(path, 1, &fd, &is_new) == QDB_OK) {
            (void)qdb__write_full(fd, garbage, QDB_HDR_SIZE, 0);
            qdb__file_close(fd);
        }
    }
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NULL(db);
        if (db) { qdb_close(db); }
    }
    test_end();

    cleanup(path);
}

static void test_invalid_header_crc(void)
{
    const char *path = "qdb_test_badcrc.qdb";

    test_begin("corrupt header CRC: qdb_open returns NULL");
    cleanup(path);

    /* Create a valid database. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) { qdb_close(db); }
    }

    /* Overwrite bytes [48..51] (flags field) with garbage so the CRC
     * stored at [52..55] no longer matches. */
    {
        uint8_t bad[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
        (void)raw_write_at(path, QDB_HDR_OFF_FLAGS, bad, 4);
    }

    {
        qdb_t *db = qdb_open(path);
        ASSERT_NULL(db);
        if (db) { qdb_close(db); }
    }
    test_end();

    cleanup(path);
}

static void test_empty_database_scan(void)
{
    const char *path = "qdb_test_empty.qdb";

    test_begin("empty database: scan finds no records");
    cleanup(path);
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            /* Scan the log manually — should be empty. */
            uint64_t offset = db->log_start_offset;
            uint8_t  type;
            uint32_t plen;
            int rc = qdb__scan_record(db->fd, &offset,
                                      db->log_end_offset, &type, &plen);
            ASSERT_EQ(rc, QDB__SCAN_END);
        }
        if (db) { qdb_close(db); }
    }
    test_end();

    cleanup(path);
}

static void test_append_and_scan_records(void)
{
    const char *path = "qdb_test_recs.qdb";
    const int   N    = 20;

    test_begin("append N records, scan them back");
    cleanup(path);

    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        /* Append N RT_PADDING records with a small payload. */
        for (int i = 0; i < N; i++) {
            uint8_t  payload[8];
            qdb__put_u64le(payload, (uint64_t)i);
            int rc = qdb__append_record(db->fd, QDB_RT_PADDING,
                                        payload, (uint32_t)sizeof(payload),
                                        &db->log_end_offset);
            ASSERT_EQ(rc, QDB_OK);
        }
        /* Persist log_end_offset so reopen sees all records. */
        (void)qdb__header_update(db->fd, db);
        qdb_close(db);
    }

    /* Reopen and scan. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (!db) { test_end(); cleanup(path); return; }

        int     count  = 0;
        uint64_t offset = db->log_start_offset;

        while (1) {
            uint8_t  type;
            uint32_t plen;
            int rc = qdb__scan_record(db->fd, &offset,
                                      db->log_end_offset, &type, &plen);
            if (rc == QDB__SCAN_END) { break; }
            ASSERT_EQ(rc, QDB_OK);
            ASSERT_EQ(type, QDB_RT_PADDING);
            ASSERT_EQ(plen, 8u);
            count++;
        }
        ASSERT_EQ(count, N);
        qdb_close(db);
    }
    test_end();

    cleanup(path);
}

static void test_partial_tail_recovery(void)
{
    const char *path = "qdb_test_partial.qdb";

    test_begin("partial tail write is truncated on reopen");
    cleanup(path);

    /* Create a valid database so log_end_offset = QDB_HDR_SIZE. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) { qdb_close(db); }
    }

    /* Extend the file with garbage bytes past log_end_offset to simulate
     * a crash mid-write. */
    {
        uint8_t junk[32];
        memset(junk, 0x55u, sizeof(junk));
        (void)raw_write_at(path, QDB_HDR_SIZE, junk, sizeof(junk));
    }

    /* File is now larger than log_end_offset. */
    ASSERT((int64_t)raw_file_size(path) > (int64_t)QDB_HDR_SIZE);

    /* Reopen: should succeed and truncate the garbage. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            ASSERT_EQ(db->log_end_offset, (uint64_t)QDB_HDR_SIZE);
            qdb_close(db);
        }
    }

    /* File should be back to exactly QDB_HDR_SIZE. */
    ASSERT_EQ(raw_file_size(path), (int64_t)QDB_HDR_SIZE);
    test_end();

    cleanup(path);
}

static void test_corrupt_record_crc(void)
{
    const char *path = "qdb_test_corr.qdb";

    test_begin("committed record with bad CRC: qdb_open returns NULL");
    cleanup(path);

    /* Create a DB and write one valid record. */
    {
        qdb_t   *db = qdb_open(path);
        uint8_t  payload[4] = { 1, 2, 3, 4 };
        ASSERT_NOTNULL(db);
        if (db) {
            int rc = qdb__append_record(db->fd, QDB_RT_PADDING,
                                        payload, 4, &db->log_end_offset);
            ASSERT_EQ(rc, QDB_OK);
            (void)qdb__header_update(db->fd, db);
            qdb_close(db);
        }
    }

    /* Flip the stored CRC bytes inside the record.
     * Record starts at QDB_HDR_SIZE.  CRC field is at offset +5 (4 bytes). */
    {
        uint8_t bad_crc[4] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu };
        (void)raw_write_at(path,
                           (uint64_t)QDB_HDR_SIZE + QDB_REC_OFF_CRC,
                           bad_crc, 4);
    }

    /* Reopen: scan must detect the CRC mismatch. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NULL(db);
        if (db) { qdb_close(db); }
    }
    test_end();

    cleanup(path);
}

static void test_commit_marker_missing(void)
{
    const char *path = "qdb_test_marker.qdb";

    test_begin("committed record with zeroed marker: truncated on reopen");
    cleanup(path);

    /* Create a DB and write one valid record. */
    uint64_t rec_end = 0;
    {
        qdb_t   *db = qdb_open(path);
        uint8_t  payload[4] = { 0xAAu, 0xBBu, 0xCCu, 0xDDu };
        ASSERT_NOTNULL(db);
        if (db) {
            uint64_t before = db->log_end_offset;
            int rc = qdb__append_record(db->fd, QDB_RT_PADDING,
                                        payload, 4, &db->log_end_offset);
            ASSERT_EQ(rc, QDB_OK);
            rec_end = db->log_end_offset;
            (void)before;
            (void)qdb__header_update(db->fd, db);
            qdb_close(db);
        }
    }

    /* Zero the commit marker (last byte of the record). */
    {
        uint8_t zero = 0x00u;
        /* marker is at rec_end - 1 */
        (void)raw_write_at(path, rec_end - 1u, &zero, 1);
    }

    /* Reopen: the record should be treated as a partial write and truncated.
     * qdb_open succeeds but the record is gone. */
    {
        qdb_t *db = qdb_open(path);
        ASSERT_NOTNULL(db);
        if (db) {
            /* log_end_offset should be back to just the header. */
            ASSERT_EQ(db->log_end_offset, (uint64_t)QDB_HDR_SIZE);
            qdb_close(db);
        }
    }
    test_end();

    cleanup(path);
}

static void test_locked_database(void)
{
    const char *path = "qdb_test_lock.qdb";

    test_begin("double-open same path: second open returns NULL");
    cleanup(path);

    qdb_t *db1 = qdb_open(path);
    ASSERT_NOTNULL(db1);

    qdb_t *db2 = qdb_open(path);  /* should fail: db1 holds the lock */
    ASSERT_NULL(db2);

    if (db1) { qdb_close(db1); }
    if (db2) { qdb_close(db2); }

    /* After close, a third open should succeed. */
    {
        qdb_t *db3 = qdb_open(path);
        ASSERT_NOTNULL(db3);
        if (db3) { qdb_close(db3); }
    }
    test_end();

    cleanup(path);
}

static void test_close_null_safe(void)
{
    test_begin("qdb_close(NULL) is a safe no-op");
    qdb_close(NULL);
    ASSERT(1);
    test_end();
}

static void test_open_null_path(void)
{
    test_begin("qdb_open(NULL) returns NULL");
    qdb_t *db = qdb_open(NULL);
    ASSERT_NULL(db);
    test_end();
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(void)
{
    printf("QDB storage tests\n");
    printf("=================\n");

    test_crc32_known_values();
    test_open_null_path();
    test_close_null_safe();
    test_open_new_database();
    test_reopen_existing_database();
    test_invalid_header_magic();
    test_invalid_header_crc();
    test_empty_database_scan();
    test_append_and_scan_records();
    test_partial_tail_recovery();
    test_corrupt_record_crc();
    test_commit_marker_missing();
    test_locked_database();

    printf("=================\n");
    printf("Results: %d/%d passed",
           g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0) {
        printf("  (%d FAILED)\n", g_tests_failed);
        return EXIT_FAILURE;
    }
    printf("\n");
    return EXIT_SUCCESS;
}
