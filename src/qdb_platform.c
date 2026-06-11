/*
 * qdb_platform.c — platform-specific I/O primitives
 *
 * Provides a uniform interface over POSIX (Linux, macOS) and Win32.
 * All functions are documented in qdb_internal.h.
 *
 * SPDX-License-Identifier: MIT
 */

/* Feature-test macros must come before any system header.
 * _DARWIN_C_SOURCE on macOS exposes F_FULLFSYNC and flock.
 * _GNU_SOURCE on Linux exposes fdatasync and flock. */
#if !defined(_WIN32)
#  if defined(__APPLE__)
#    if !defined(_DARWIN_C_SOURCE)
#      define _DARWIN_C_SOURCE
#    endif
#  else
#    if !defined(_GNU_SOURCE)
#      define _GNU_SOURCE
#    endif
#  endif
#  if !defined(_FILE_OFFSET_BITS)
#    define _FILE_OFFSET_BITS 64
#  endif
#endif

#include "qdb_internal.h"

/* =========================================================================
 * POSIX implementation
 * ====================================================================== */
#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>     /* rename */
#include <sys/file.h>  /* flock */
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* macOS requires F_FULLFSYNC for a true hardware flush. */
#if defined(__APPLE__)
#  include <fcntl.h>
#  define QDB_PLATFORM_MACOS 1
#endif

/* ---------------------------------------------------------------------- */

int qdb__file_open(const char *path, int create,
                   qdb__fd_t *out_fd, int *out_is_new)
{
    int fd;

    /* Try exclusive create first to detect new vs. existing. */
    if (create) {
        fd = open(path, O_RDWR | O_CREAT | O_EXCL, (mode_t)0600);
        if (fd >= 0) {
            *out_fd    = (qdb__fd_t)fd;
            *out_is_new = 1;
            return QDB_OK;
        }
        if (errno != EEXIST) {
            return QDB_ERR_IO;
        }
    }

    /* Open existing file. */
    fd = open(path, O_RDWR);
    if (fd < 0) {
        return QDB_ERR_IO;
    }
    *out_fd    = (qdb__fd_t)fd;
    *out_is_new = 0;
    return QDB_OK;
}

int qdb__lockfile_open(const char *path, qdb__fd_t *out_fd)
{
    int fd = open(path, O_RDWR | O_CREAT, (mode_t)0600);
    if (fd < 0) {
        return QDB_ERR_IO;
    }
    *out_fd = (qdb__fd_t)fd;
    return QDB_OK;
}

void qdb__file_close(qdb__fd_t fd)
{
    (void)close((int)fd);
}

int qdb__file_sync(qdb__fd_t fd)
{
#if defined(QDB_PLATFORM_MACOS)
    if (fcntl((int)fd, F_FULLFSYNC) == 0) {
        return QDB_OK;
    }
    /* F_FULLFSYNC failed (e.g. on a non-HFS+ volume); fall back to fsync. */
    if (fsync((int)fd) == 0) {
        return QDB_OK;
    }
#else
    if (fdatasync((int)fd) == 0) {
        return QDB_OK;
    }
#endif
    return QDB_ERR_IO;
}

int qdb__file_size(qdb__fd_t fd, uint64_t *out_size)
{
    off_t pos = lseek((int)fd, 0, SEEK_END);
    if (pos < 0) {
        return QDB_ERR_IO;
    }
    *out_size = (uint64_t)pos;
    return QDB_OK;
}

int qdb__file_truncate(qdb__fd_t fd, uint64_t size)
{
    if (ftruncate((int)fd, (off_t)size) != 0) {
        return QDB_ERR_IO;
    }
    return QDB_OK;
}

int qdb__pread(qdb__fd_t fd, void *buf, size_t len,
               uint64_t offset, size_t *out_nread)
{
    uint8_t *p       = (uint8_t *)buf;
    size_t   total   = 0;

    while (total < len) {
        ssize_t n = pread((int)fd, p + total, len - total, (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return QDB_ERR_IO;
        }
        if (n == 0) {
            break; /* EOF */
        }
        total += (size_t)n;
    }
    *out_nread = total;
    return QDB_OK;
}

int qdb__pwrite(qdb__fd_t fd, const void *buf, size_t len, uint64_t offset)
{
    const uint8_t *p     = (const uint8_t *)buf;
    size_t         total = 0;

    while (total < len) {
        ssize_t n = pwrite((int)fd, p + total, len - total, (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return QDB_ERR_IO;
        }
        if (n == 0) {
            return QDB_ERR_IO;
        }
        total += (size_t)n;
    }
    return QDB_OK;
}

int qdb__file_lock(qdb__fd_t fd)
{
    if (flock((int)fd, LOCK_EX | LOCK_NB) == 0) {
        return QDB_OK;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return QDB_ERR_LOCKED;
    }
    return QDB_ERR_IO;
}

void qdb__file_unlock(qdb__fd_t fd)
{
    (void)flock((int)fd, LOCK_UN);
}

int qdb__file_delete(const char *path)
{
    if (unlink(path) == 0 || errno == ENOENT) {
        return QDB_OK;
    }
    return QDB_ERR_IO;
}

int qdb__file_rename(const char *old_path, const char *new_path)
{
    if (rename(old_path, new_path) == 0) {
        return QDB_OK;
    }
    return QDB_ERR_IO;
}

uint64_t qdb__time_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

/* =========================================================================
 * Windows implementation
 * ====================================================================== */
#else /* _WIN32 */

/* Suppress deprecation warnings for POSIX names in MSVC. */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE

#include <windows.h>
#include <fileapi.h>
#include <io.h>

/* Map intptr_t fd to HANDLE. */
#define FD2H(fd)  ((HANDLE)(fd))
#define H2FD(h)   ((qdb__fd_t)(intptr_t)(h))

static int qdb__win_open(const char *path, DWORD access, DWORD disposition,
                         qdb__fd_t *out_fd)
{
    HANDLE h = CreateFileA(
        path,
        access,
        0,                     /* no sharing */
        NULL,
        disposition,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL
    );
    if (h == INVALID_HANDLE_VALUE) {
        return QDB_ERR_IO;
    }
    *out_fd = H2FD(h);
    return QDB_OK;
}

int qdb__file_open(const char *path, int create,
                   qdb__fd_t *out_fd, int *out_is_new)
{
    DWORD access = GENERIC_READ | GENERIC_WRITE;

    if (create) {
        /* CREATE_NEW fails if exists → we know it's new. */
        HANDLE h = CreateFileA(path, access, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            *out_fd    = H2FD(h);
            *out_is_new = 1;
            return QDB_OK;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return QDB_ERR_IO;
        }
    }

    /* Open existing. */
    HANDLE h = CreateFileA(path, access, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return QDB_ERR_IO;
    }
    *out_fd    = H2FD(h);
    *out_is_new = 0;
    return QDB_OK;
}

int qdb__lockfile_open(const char *path, qdb__fd_t *out_fd)
{
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return QDB_ERR_IO;
    }
    *out_fd = H2FD(h);
    return QDB_OK;
}

void qdb__file_close(qdb__fd_t fd)
{
    CloseHandle(FD2H(fd));
}

int qdb__file_sync(qdb__fd_t fd)
{
    return FlushFileBuffers(FD2H(fd)) ? QDB_OK : QDB_ERR_IO;
}

int qdb__file_size(qdb__fd_t fd, uint64_t *out_size)
{
    LARGE_INTEGER li;
    if (!GetFileSizeEx(FD2H(fd), &li)) {
        return QDB_ERR_IO;
    }
    *out_size = (uint64_t)li.QuadPart;
    return QDB_OK;
}

int qdb__file_truncate(qdb__fd_t fd, uint64_t size)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(FD2H(fd), li, NULL, FILE_BEGIN)) {
        return QDB_ERR_IO;
    }
    return SetEndOfFile(FD2H(fd)) ? QDB_OK : QDB_ERR_IO;
}

int qdb__pread(qdb__fd_t fd, void *buf, size_t len,
               uint64_t offset, size_t *out_nread)
{
    OVERLAPPED ol;
    DWORD      got = 0;

    memset(&ol, 0, sizeof(ol));
    ol.Offset     = (DWORD)(offset & 0xFFFFFFFFu);
    ol.OffsetHigh = (DWORD)(offset >> 32);

    if (!ReadFile(FD2H(fd), buf, (DWORD)len, &got, &ol)) {
        DWORD err = GetLastError();
        if (err != ERROR_HANDLE_EOF) {
            return QDB_ERR_IO;
        }
    }
    *out_nread = (size_t)got;
    return QDB_OK;
}

int qdb__pwrite(qdb__fd_t fd, const void *buf, size_t len, uint64_t offset)
{
    OVERLAPPED ol;
    DWORD      written = 0;

    memset(&ol, 0, sizeof(ol));
    ol.Offset     = (DWORD)(offset & 0xFFFFFFFFu);
    ol.OffsetHigh = (DWORD)(offset >> 32);

    if (!WriteFile(FD2H(fd), buf, (DWORD)len, &written, &ol)) {
        return QDB_ERR_IO;
    }
    return (written == (DWORD)len) ? QDB_OK : QDB_ERR_IO;
}

int qdb__file_lock(qdb__fd_t fd)
{
    OVERLAPPED ol;
    memset(&ol, 0, sizeof(ol));
    if (LockFileEx(FD2H(fd),
                   LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, 1, 0, &ol)) {
        return QDB_OK;
    }
    DWORD err = GetLastError();
    if (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING) {
        return QDB_ERR_LOCKED;
    }
    return QDB_ERR_IO;
}

void qdb__file_unlock(qdb__fd_t fd)
{
    OVERLAPPED ol;
    memset(&ol, 0, sizeof(ol));
    (void)UnlockFileEx(FD2H(fd), 0, 1, 0, &ol);
}

int qdb__file_delete(const char *path)
{
    if (DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND) {
        return QDB_OK;
    }
    return QDB_ERR_IO;
}

int qdb__file_rename(const char *old_path, const char *new_path)
{
    if (MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING)) {
        return QDB_OK;
    }
    return QDB_ERR_IO;
}

uint64_t qdb__time_us(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* FILETIME: 100-nanosecond intervals since 1601-01-01. */
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    /* Offset to Unix epoch (Jan 1, 1601 → Jan 1, 1970): 11644473600 seconds */
    t -= 116444736000000000ull;
    return t / 10u; /* convert 100-ns ticks to microseconds */
}

#endif /* _WIN32 */
