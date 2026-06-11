/*
 * qdb_state.h — in-memory queue state data structures
 *
 * Defines the runtime representation of messages, queues, and leases
 * reconstructed from the append-only log during qdb_open().
 *
 * Not installed.  Must not be included by application code.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QDB_STATE_H
#define QDB_STATE_H

#include "qdb_internal.h"

/* -------------------------------------------------------------------------
 * Message lifecycle state
 * ---------------------------------------------------------------------- */

typedef enum {
    QDB_MSG_STATE_PENDING = 0,  /* ready to be consumed      */
    QDB_MSG_STATE_LEASED  = 1,  /* held by an active lease   */
    QDB_MSG_STATE_ACKED   = 2,  /* permanently consumed      */
    QDB_MSG_STATE_DEAD    = 3   /* retry limit exceeded (future) */
} qdb_msg_state_t;

/* -------------------------------------------------------------------------
 * Message entry
 *
 * One entry per RT_MSG_PUSH record.  Kept for the lifetime of the db
 * handle.  Only PENDING and LEASED messages participate in the active
 * queue; ACKED messages are retained for ID deduplication during replay.
 * ---------------------------------------------------------------------- */

struct qdb__msg {
    uint64_t         id;
    uint64_t         data_file_offset;  /* byte offset of message data in main file */
    uint32_t         data_len;          /* length of message data in bytes */
    uint8_t          queue_name_len;
    char             queue_name[QDB_QUEUE_NAME_MAX + 1]; /* NUL-terminated */
    qdb_msg_state_t  state;
    uint64_t         lease_id;          /* current lease; 0 = not leased */
    uint64_t         lease_expiry_us;   /* expiry timestamp; 0 = not leased */
    uint32_t         retry_count;       /* incremented by each NACK/EXPIRE */

    /* Intrusive doubly-linked list within the owning queue's PENDING chain.
     * Values are msg_id; 0 is the "no neighbour" sentinel (ID 0 is reserved). */
    uint64_t         next_pending;
    uint64_t         prev_pending;

    /* Hash table chaining */
    struct qdb__msg *next_in_bucket;
};

/* -------------------------------------------------------------------------
 * Queue entry
 * ---------------------------------------------------------------------- */

struct qdb__queue {
    char               name[QDB_QUEUE_NAME_MAX + 1]; /* NUL-terminated */
    uint8_t            name_len;
    uint32_t           pending_count;
    uint32_t           leased_count;
    uint32_t           acked_count;
    uint64_t           pending_head;    /* msg_id of first PENDING; 0 = empty */
    uint64_t           pending_tail;    /* msg_id of last PENDING;  0 = empty */
    struct qdb__queue *next_in_bucket;
};

/* -------------------------------------------------------------------------
 * Lease entry
 * ---------------------------------------------------------------------- */

struct qdb__lease {
    uint64_t            lease_id;
    uint64_t            msg_id;
    uint64_t            expiry_us;
    struct qdb__lease  *next_in_bucket;
};

/* -------------------------------------------------------------------------
 * Hash table dimensions (power-of-2 for cheap modulo masking)
 * ---------------------------------------------------------------------- */

#define QDB__MSG_BUCKETS   1024u
#define QDB__QUEUE_BUCKETS   64u
#define QDB__LEASE_BUCKETS 1024u

/* -------------------------------------------------------------------------
 * In-memory state
 * ---------------------------------------------------------------------- */

struct qdb__state {
    struct qdb__msg   *msg_buckets[QDB__MSG_BUCKETS];
    struct qdb__queue *queue_buckets[QDB__QUEUE_BUCKETS];
    struct qdb__lease *lease_buckets[QDB__LEASE_BUCKETS];

    uint64_t msg_count;
    uint64_t queue_count;
    uint64_t lease_count;
};

/* -------------------------------------------------------------------------
 * State lifecycle
 * ---------------------------------------------------------------------- */

struct qdb__state *qdb__state_alloc(void);
void               qdb__state_free(struct qdb__state *st);

/* -------------------------------------------------------------------------
 * Message table
 * ---------------------------------------------------------------------- */

struct qdb__msg *qdb__msg_get(struct qdb__state *st, uint64_t id);

/*
 * Insert m into the message table.  m must not already be present
 * (duplicate check is the caller's responsibility).
 * Returns QDB_OK or QDB_ERR_NOMEM (unreachable with chained hashing, but
 * kept for a uniform error-code contract).
 */
int qdb__msg_insert(struct qdb__state *st, struct qdb__msg *m);

/* -------------------------------------------------------------------------
 * Queue table
 * ---------------------------------------------------------------------- */

struct qdb__queue *qdb__queue_get(struct qdb__state *st,
                                   const char *name, uint8_t name_len);

/*
 * Return the queue, creating it if it does not yet exist.
 * Returns NULL on OOM.
 */
struct qdb__queue *qdb__queue_get_or_create(struct qdb__state *st,
                                             const char *name, uint8_t name_len);

/* Append msg to the tail of queue q's PENDING list. */
void qdb__queue_pending_append(struct qdb__state *st,
                                struct qdb__queue *q, struct qdb__msg *m);

/* Remove msg from queue q's PENDING list (used when a message is leased). */
void qdb__queue_pending_remove(struct qdb__state *st,
                                struct qdb__queue *q, struct qdb__msg *m);

/* -------------------------------------------------------------------------
 * Lease table
 * ---------------------------------------------------------------------- */

struct qdb__lease *qdb__lease_get(struct qdb__state *st, uint64_t lease_id);

/*
 * Insert l into the lease table.
 * Returns QDB_OK or QDB_ERR_NOMEM (same note as qdb__msg_insert).
 */
int  qdb__lease_insert(struct qdb__state *st, struct qdb__lease *l);
void qdb__lease_remove(struct qdb__state *st, uint64_t lease_id);

/* -------------------------------------------------------------------------
 * Iteration API (for tests only — not part of the stable internal ABI)
 * ---------------------------------------------------------------------- */

typedef void (*qdb__msg_iter_fn)(const struct qdb__msg   *m, void *ctx);
typedef void (*qdb__queue_iter_fn)(const struct qdb__queue *q, void *ctx);

void qdb__state_iter_msgs(  struct qdb__state *st, qdb__msg_iter_fn   fn, void *ctx);
void qdb__state_iter_queues(struct qdb__state *st, qdb__queue_iter_fn fn, void *ctx);

#endif /* QDB_STATE_H */
