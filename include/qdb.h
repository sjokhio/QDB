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

/** The message ID passed to qdb_ack does not match any leased message,
 *  or the message has already been acknowledged. */
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
 * ---------------------------------------------------------------------- */

/**
 * Descriptor for a message returned by qdb_pop().
 *
 * All pointer fields are heap-allocated.  The caller owns the struct
 * after a successful qdb_pop() and must release it with qdb_msg_free().
 * Zero-initialising a qdb_msg_t (= {0} or memset to 0) is always safe.
 *
 * @id        Opaque monotonic message identifier.  Pass to qdb_ack().
 * @lease_id  Identifier of the lease granted by qdb_pop().  Exposed for
 *            diagnostics and future lease-aware qdb_ack() variants.
 * @queue     Heap-allocated, null-terminated name of the source queue.
 * @data      Heap-allocated copy of the raw message payload.
 *            NULL when @len is zero.
 * @len       Length of @data in bytes.
 */
typedef struct {
    uint64_t  id;
    uint64_t  lease_id;
    char     *queue;
    void     *data;
    size_t    len;
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
 * @data   Payload to enqueue.  May be NULL only when @len is zero.
 * @len    Length of @data in bytes.  Pass zero for an empty message.
 *
 * @return  QDB_OK on success.
 *          QDB_ERR_INVAL if any argument is invalid.
 *          QDB_ERR_IO    on a filesystem or flush failure.
 *          QDB_ERR_NOMEM if an internal allocation fails.
 */
int qdb_push(qdb_t *db, const char *queue, const void *data, size_t len);

/**
 * qdb_msg_free — release the resources owned by a qdb_msg_t.
 *
 * Frees the heap-allocated @queue and @data fields and zeroes the struct
 * so it is safe to call qdb_msg_free() on the same pointer again
 * (idempotent).  It is safe to pass a pointer to a zero-initialised
 * qdb_msg_t.
 *
 * @msg  Pointer to the descriptor to release.  Must not be NULL.
 */
void qdb_msg_free(qdb_msg_t *msg);

/**
 * qdb_pop — dequeue the next available message from a queue.
 *
 * Takes the oldest PENDING message from @queue, grants it a time-bounded
 * lease, and writes the result into *@out_msg.  The message transitions to
 * LEASED state and will not be returned by subsequent qdb_pop() calls
 * until the lease expires or is resolved.
 *
 * On success *@out_msg is populated with heap-allocated copies of the
 * queue name and message payload.  The caller must release these with
 * qdb_msg_free() when done.
 *
 * Call qdb_ack() with out_msg->id to permanently remove the message.
 * Messages whose leases expire without an acknowledgement are automatically
 * redelivered on the next qdb_pop(), providing at-least-once semantics.
 *
 * @db       Open database handle.  Must not be NULL.
 * @queue    Name of the source queue.  Must not be NULL or empty.
 * @out_msg  Output parameter.  Populated on success.  Must not be NULL.
 *           The caller owns the contents after a QDB_OK return.
 *
 * @return  QDB_OK         on success; *@out_msg is populated.
 *          QDB_ERR_EMPTY  if @queue has no available messages.
 *          QDB_ERR_INVAL  if any argument is invalid.
 *          QDB_ERR_IO     on a read or write failure.
 *          QDB_ERR_NOMEM  if a heap allocation fails.
 */
int qdb_pop(qdb_t *db, const char *queue, qdb_msg_t *out_msg);

/**
 * qdb_ack — acknowledge and permanently delete a leased message.
 *
 * Marks the message identified by @msg_id as permanently consumed.  The
 * message is removed from the active queue and will not be redelivered.
 * Both @msg_id and @lease_id must match the qdb_msg_t returned by the
 * qdb_pop() that granted the lease.
 *
 * @db        Open database handle.  Must not be NULL.
 * @msg_id    Message identifier: qdb_msg_t.id.
 * @lease_id  Lease identifier: qdb_msg_t.lease_id.  Must match the active
 *            lease on the message; guards against stale acknowledgements
 *            after a lease has expired and been re-granted to another caller.
 *
 * @return  QDB_OK         on success.
 *          QDB_ERR_INVAL  if @db is NULL, or @lease_id does not match the
 *                         active lease on @msg_id.
 *          QDB_ERR_NOENT  if @msg_id does not exist, has already been
 *                         acknowledged, or is not currently leased.
 *          QDB_ERR_IO     on a flush failure.
 */
int qdb_ack(qdb_t *db, uint64_t msg_id, uint64_t lease_id);

/**
 * qdb_nack — return a leased message to the queue without consuming it.
 *
 * Releases the lease on the message identified by @msg_id and returns it
 * to the tail of its source queue as a PENDING message.  The message will
 * be redelivered by the next qdb_pop() on that queue, providing
 * at-least-once delivery semantics.
 *
 * Use qdb_nack() when a consumer encounters a transient error and cannot
 * process the message right now but wants it to be retried.  Use
 * qdb_ack() to permanently remove a successfully processed message.
 *
 * @db        Open database handle.  Must not be NULL.
 * @msg_id    Message identifier: qdb_msg_t.id.
 * @lease_id  Lease identifier: qdb_msg_t.lease_id.  Must match the active
 *            lease on the message.
 *
 * @return  QDB_OK         on success; the message is back in the queue.
 *          QDB_ERR_INVAL  if @db is NULL, or @lease_id does not match the
 *                         active lease on @msg_id.
 *          QDB_ERR_NOENT  if @msg_id does not exist, is not currently
 *                         leased, or has already been acknowledged.
 *          QDB_ERR_IO     on a flush failure.
 */
int qdb_nack(qdb_t *db, uint64_t msg_id, uint64_t lease_id);

/**
 * qdb_process_expired_leases — expire overdue leases and requeue messages.
 *
 * Scans all active leases and, for each whose deadline has passed, writes a
 * durable RT_MSG_EXPIRE record and returns the message to the tail of its
 * source queue as a PENDING message.  The message's retry_count is
 * incremented.
 *
 * QDB has no background thread.  The application must call this function
 * explicitly — before each qdb_pop(), or on a periodic timer — to ensure
 * expired leases are processed in a timely fashion.
 *
 * Partial progress: if a disk write fails mid-scan, leases already expired
 * in this call retain their new PENDING state; the failing lease and any
 * leases not yet reached remain active.
 *
 * @db  Open database handle.  Must not be NULL.
 *
 * @return  Number of leases expired (>= 0) on success.
 *          QDB_ERR_INVAL  if @db is NULL.
 *          QDB_ERR_IO     if a durable write fails.
 */
int qdb_process_expired_leases(qdb_t *db);

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
