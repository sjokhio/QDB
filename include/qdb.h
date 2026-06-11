/*
 * qdb.h — QDB public API
 *
 * QDB is a lightweight embedded persistent message queue library.
 * It provides durable, crash-safe named queues backed by an append-only
 * log file.  There is no server process and no external dependencies
 * beyond libc.
 *
 * Thread safety: a qdb_t handle must not be shared between threads
 * without external synchronisation.  Opening separate handles to the
 * same database file from multiple threads or processes is not supported
 * in v1.
 *
 * Error codes: every fallible function returns QDB_OK (0) on success or
 * a negative QDB_ERR_* constant on failure.  Functions that return a
 * pointer return NULL on failure.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QDB_H
#define QDB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------------- */

#define QDB_VERSION_MAJOR 0
#define QDB_VERSION_MINOR 1
#define QDB_VERSION_PATCH 0

#define QDB_VERSION_NUMBER \
    ((QDB_VERSION_MAJOR) * 10000 + (QDB_VERSION_MINOR) * 100 + (QDB_VERSION_PATCH))

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */

/** Success. */
#define QDB_OK            0

/** Unspecified I/O error.  Inspect errno for details. */
#define QDB_ERR_IO       (-1)

/** The database file is corrupt or has an unrecognised format. */
#define QDB_ERR_CORRUPT  (-2)

/** An argument passed by the caller is invalid (NULL pointer, empty
 *  queue name, zero-length message, etc.). */
#define QDB_ERR_INVAL    (-3)

/** The requested queue is empty; there is no message to pop. */
#define QDB_ERR_EMPTY    (-4)

/** The message ID passed to qdb_ack does not match any pending message. */
#define QDB_ERR_NOENT    (-5)

/** A memory allocation failed. */
#define QDB_ERR_NOMEM    (-6)

/** The database file is locked by another process. */
#define QDB_ERR_LOCKED   (-7)

/* -------------------------------------------------------------------------
 * Opaque handle
 * ---------------------------------------------------------------------- */

/**
 * Opaque database handle.
 *
 * Obtain one with qdb_open() and release it with qdb_close().  Do not
 * copy, stack-allocate, or dereference this type directly.
 */
typedef struct qdb qdb_t;

/* -------------------------------------------------------------------------
 * Message descriptor
 *
 * Returned by qdb_pop.  The caller must NOT free msg.data; the buffer is
 * owned by the qdb_t handle and is valid until the next call that takes
 * the same handle.
 * ---------------------------------------------------------------------- */

/**
 * Descriptor for a message returned by qdb_pop().
 *
 * @id    Opaque message identifier, required by qdb_ack().
 * @data  Pointer to the raw message payload.  Valid until the next call
 *        on this handle.  Do not free.
 * @len   Length of @data in bytes.
 */
typedef struct {
    uint64_t    id;
    const void *data;
    size_t      len;
} qdb_msg_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * qdb_open — open (or create) a queue database.
 *
 * Opens the database file at @path.  If the file does not exist it is
 * created and initialised.  If the file exists and contains a valid QDB
 * database, it is opened for use.
 *
 * Recovery: if a previous run crashed without flushing the write-ahead
 * log, qdb_open() performs crash recovery automatically before returning.
 *
 * @path   Filesystem path to the database file.  Must not be NULL.
 *
 * @return  Pointer to a new qdb_t handle on success.
 *          NULL on failure (QDB_ERR_IO, QDB_ERR_CORRUPT, QDB_ERR_NOMEM,
 *          or QDB_ERR_LOCKED).
 *
 * The caller is responsible for calling qdb_close() when done.
 */
qdb_t *qdb_open(const char *path);

/**
 * qdb_close — flush and close a queue database.
 *
 * Flushes all pending writes, releases the file lock, and frees all
 * resources associated with @db.  After this call @db is invalid and
 * must not be used.
 *
 * It is safe to pass NULL; the call is a no-op in that case.
 *
 * @db  Handle returned by qdb_open().
 */
void qdb_close(qdb_t *db);

/* -------------------------------------------------------------------------
 * Queue operations
 * ---------------------------------------------------------------------- */

/**
 * qdb_push — append a message to a named queue.
 *
 * Durably appends a copy of @data (of @len bytes) to the queue named
 * @queue.  If the queue does not yet exist it is created implicitly.
 *
 * The write is crash-safe: if the process is killed after qdb_push()
 * returns QDB_OK, the message will be present after the next qdb_open()
 * and recovery.
 *
 * @db     Open database handle.  Must not be NULL.
 * @queue  Name of the target queue.  Must be a non-empty, null-terminated
 *         string no longer than QDB_QUEUE_NAME_MAX bytes.
 * @data   Payload to enqueue.  Must not be NULL.
 * @len    Length of @data in bytes.  Must be greater than zero.
 *
 * @return  QDB_OK on success.
 *          QDB_ERR_INVAL if any argument is invalid.
 *          QDB_ERR_IO    on a filesystem or flush failure.
 *          QDB_ERR_NOMEM if an internal allocation fails.
 */
int qdb_push(qdb_t *db, const char *queue, const void *data, size_t len);

/**
 * qdb_pop — dequeue the next unacknowledged message.
 *
 * Removes the oldest unacknowledged message from @queue and writes its
 * descriptor into *@msg.  The message transitions to a "pending
 * acknowledgement" state; it will not be returned by subsequent
 * qdb_pop() calls but it is not yet deleted from durable storage.
 *
 * Call qdb_ack() with msg.id to commit the deletion.  Messages that are
 * never acknowledged (e.g. because the process crashed) will be
 * redelivered after recovery, providing at-least-once semantics.
 *
 * @db     Open database handle.  Must not be NULL.
 * @queue  Name of the source queue.  Must not be NULL or empty.
 * @msg    Output parameter.  Populated on success.  Must not be NULL.
 *
 * @return  QDB_OK    on success; *@msg is populated.
 *          QDB_ERR_EMPTY  if @queue has no unacknowledged messages.
 *          QDB_ERR_INVAL  if any argument is invalid.
 *          QDB_ERR_IO     on a read failure.
 */
int qdb_pop(qdb_t *db, const char *queue, qdb_msg_t *msg);

/**
 * qdb_ack — acknowledge and permanently delete a message.
 *
 * Marks the message identified by @msg_id as permanently consumed.  The
 * message is removed from the database and will not be redelivered.
 *
 * @msg_id must be the id field from a qdb_msg_t previously returned by
 * qdb_pop() on the same handle, and must not have been acknowledged
 * already.
 *
 * @db      Open database handle.  Must not be NULL.
 * @msg_id  Message identifier from qdb_msg_t.id.
 *
 * @return  QDB_OK        on success.
 *          QDB_ERR_NOENT if @msg_id does not match any pending message.
 *          QDB_ERR_IO    on a flush failure.
 */
int qdb_ack(qdb_t *db, uint64_t msg_id);

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */

/** Maximum length of a queue name, excluding the null terminator. */
#define QDB_QUEUE_NAME_MAX 255

/** Maximum message payload size (64 MiB). */
#define QDB_MSG_MAX_LEN    (64u * 1024u * 1024u)

/* -------------------------------------------------------------------------
 * Utilities
 * ---------------------------------------------------------------------- */

/**
 * qdb_errmsg — return a human-readable description of an error code.
 *
 * The returned string is a string literal; do not free it.
 *
 * @err  A QDB_ERR_* constant (or QDB_OK).
 *
 * @return  Pointer to a null-terminated description string.
 */
const char *qdb_errmsg(int err);

/**
 * qdb_version — return the library version as a string.
 *
 * @return  Pointer to a null-terminated string such as "0.1.0".
 *          The string is a literal; do not free it.
 */
const char *qdb_version(void);

#ifdef __cplusplus
}
#endif

#endif /* QDB_H */
