/*
 * test_platform.h — portable test helpers for QDB tests
 *
 * Provides a small portability layer for Windows and POSIX test code.
 * SPDX-License-Identifier: MIT
 */

#ifndef QDB_TEST_PLATFORM_H
#define QDB_TEST_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS
#  endif
#  ifndef _CRT_NONSTDC_NO_DEPRECATE
#    define _CRT_NONSTDC_NO_DEPRECATE
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  if defined(QDB_TEST_PLATFORM_RAW_IO)
#    include <fcntl.h>
#    include <sys/stat.h>
#  endif
#  include <unistd.h>
#endif

static inline int qdb_test_remove_file(const char *path)
{
#if defined(_WIN32)
    if (DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND) {
        return 0;
    }
    return -1;
#else
    if (unlink(path) == 0 || errno == ENOENT) {
        return 0;
    }
    return -1;
#endif
}

static inline void qdb_test_cleanup_files(const char *path)
{
    char sidecar[512];
    size_t plen = strlen(path);

    (void)qdb_test_remove_file(path);
    if (plen + 6 < sizeof(sidecar)) {
        memcpy(sidecar, path, plen);
        memcpy(sidecar + plen, "-wal", 5);
        (void)qdb_test_remove_file(sidecar);
        memcpy(sidecar + plen, "-lock", 6);
        (void)qdb_test_remove_file(sidecar);
    }
}

static inline int qdb_test_close_fd(intptr_t fd)
{
#if defined(_WIN32)
    /* QDB stores native Win32 HANDLEs in qdb__fd_t, not CRT descriptors. */
    return CloseHandle((HANDLE)fd) ? 0 : -1;
#else
    return close((int)fd);
#endif
}

#if defined(QDB_TEST_PLATFORM_RAW_IO)
static inline int qdb_test_raw_write_at(const char *path, uint64_t offset,
                                        const void *buf, size_t len)
{
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER position;
    DWORD written = 0;

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, position, NULL, FILE_BEGIN) ||
        !WriteFile(h, buf, (DWORD)len, &written, NULL)) {
        (void)CloseHandle(h);
        return -1;
    }
    (void)CloseHandle(h);
    return ((size_t)written == len) ? 0 : -1;
#else
    int fd = open(path, O_WRONLY);
    ssize_t written;

    if (fd < 0) {
        return -1;
    }
    written = pwrite(fd, buf, len, (off_t)offset);
    (void)close(fd);
    return (written >= 0 && (size_t)written == len) ? 0 : -1;
#endif
}

static inline int64_t qdb_test_raw_file_size(const char *path)
{
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!GetFileSizeEx(h, &size)) {
        (void)CloseHandle(h);
        return -1;
    }
    (void)CloseHandle(h);
    return (int64_t)size.QuadPart;
#else
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }
    return (int64_t)st.st_size;
#endif
}

static inline int64_t qdb_test_raw_file_size_fd(intptr_t fd)
{
#if defined(_WIN32)
    LARGE_INTEGER size;

    if (!GetFileSizeEx((HANDLE)fd, &size)) {
        return -1;
    }
    return (int64_t)size.QuadPart;
#else
    struct stat st;

    if (fstat((int)fd, &st) != 0) {
        return -1;
    }
    return (int64_t)st.st_size;
#endif
}
#endif

#endif /* QDB_TEST_PLATFORM_H */
