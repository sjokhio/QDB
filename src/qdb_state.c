/*
 * qdb_state.c — in-memory queue state management
 *
 * Hash tables for messages, queues, and leases.  All tables use
 * separate chaining (linked lists within each bucket) for simplicity.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb_state.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Hash functions
 * ---------------------------------------------------------------------- */

/* Fibonacci hashing for 64-bit integer keys (Knuth multiplicative).
 * Produces a bucket index in [0, QDB__MSG_BUCKETS). */
static uint32_t msg_bucket(uint64_t id)
{
    uint64_t h = id * 11400714819323198485ull;
    return (uint32_t)(h >> (64u - 10u)); /* 2^10 = 1024 = QDB__MSG_BUCKETS */
}

static uint32_t lease_bucket(uint64_t id)
{
    uint64_t h = id * 11400714819323198485ull;
    return (uint32_t)(h >> (64u - 10u)); /* 2^10 = 1024 = QDB__LEASE_BUCKETS */
}

/* FNV-1a over the queue name bytes. */
static uint32_t queue_bucket(const char *name, uint8_t name_len)
{
    uint32_t h = 2166136261u;
    uint8_t  i;
    for (i = 0; i < name_len; i++) {
        h ^= (uint32_t)(uint8_t)name[(size_t)i];
        h *= 16777619u;
    }
    return h & (uint32_t)(QDB__QUEUE_BUCKETS - 1u);
}

/* -------------------------------------------------------------------------
 * State lifecycle
 * ---------------------------------------------------------------------- */

struct qdb__state *qdb__state_alloc(void)
{
    struct qdb__state *st = (struct qdb__state *)calloc(1, sizeof(*st));
    return st; /* NULL on OOM */
}

void qdb__state_free(struct qdb__state *st)
{
    uint32_t i;

    if (!st) {
        return;
    }

    /* Free all message entries. */
    for (i = 0; i < QDB__MSG_BUCKETS; i++) {
        struct qdb__msg *m = st->msg_buckets[i];
        while (m) {
            struct qdb__msg *next = m->next_in_bucket;
            free(m);
            m = next;
        }
    }

    /* Free all queue entries. */
    for (i = 0; i < QDB__QUEUE_BUCKETS; i++) {
        struct qdb__queue *q = st->queue_buckets[i];
        while (q) {
            struct qdb__queue *next = q->next_in_bucket;
            free(q);
            q = next;
        }
    }

    /* Free all lease entries. */
    for (i = 0; i < QDB__LEASE_BUCKETS; i++) {
        struct qdb__lease *l = st->lease_buckets[i];
        while (l) {
            struct qdb__lease *next = l->next_in_bucket;
            free(l);
            l = next;
        }
    }

    free(st);
}

/* -------------------------------------------------------------------------
 * Message table
 * ---------------------------------------------------------------------- */

struct qdb__msg *qdb__msg_get(struct qdb__state *st, uint64_t id)
{
    uint32_t         b = msg_bucket(id);
    struct qdb__msg *m = st->msg_buckets[b];
    while (m) {
        if (m->id == id) { return m; }
        m = m->next_in_bucket;
    }
    return NULL;
}

int qdb__msg_insert(struct qdb__state *st, struct qdb__msg *m)
{
    uint32_t b            = msg_bucket(m->id);
    m->next_in_bucket     = st->msg_buckets[b];
    st->msg_buckets[b]    = m;
    st->msg_count++;
    return QDB_OK;
}

/* -------------------------------------------------------------------------
 * Queue table
 * ---------------------------------------------------------------------- */

struct qdb__queue *qdb__queue_get(struct qdb__state *st,
                                   const char *name, uint8_t name_len)
{
    uint32_t           b = queue_bucket(name, name_len);
    struct qdb__queue *q = st->queue_buckets[b];
    while (q) {
        if (q->name_len == name_len &&
            memcmp(q->name, name, (size_t)name_len) == 0) {
            return q;
        }
        q = q->next_in_bucket;
    }
    return NULL;
}

struct qdb__queue *qdb__queue_get_or_create(struct qdb__state *st,
                                             const char *name, uint8_t name_len)
{
    struct qdb__queue *q = qdb__queue_get(st, name, name_len);
    uint32_t           b;

    if (q) { return q; }

    q = (struct qdb__queue *)calloc(1, sizeof(*q));
    if (!q) { return NULL; }

    memcpy(q->name, name, (size_t)name_len);
    q->name[(size_t)name_len] = '\0';
    q->name_len               = name_len;

    b                       = queue_bucket(name, name_len);
    q->next_in_bucket       = st->queue_buckets[b];
    st->queue_buckets[b]    = q;
    st->queue_count++;
    return q;
}

void qdb__queue_pending_append(struct qdb__state *st,
                                struct qdb__queue *q, struct qdb__msg *m)
{
    m->next_pending = 0;
    m->prev_pending = q->pending_tail;

    if (q->pending_tail != 0) {
        struct qdb__msg *tail = qdb__msg_get(st, q->pending_tail);
        if (tail) { tail->next_pending = m->id; }
    } else {
        q->pending_head = m->id;
    }
    q->pending_tail = m->id;
    q->pending_count++;
}

void qdb__queue_pending_remove(struct qdb__state *st,
                                struct qdb__queue *q, struct qdb__msg *m)
{
    if (m->prev_pending != 0) {
        struct qdb__msg *prev = qdb__msg_get(st, m->prev_pending);
        if (prev) { prev->next_pending = m->next_pending; }
    } else {
        q->pending_head = m->next_pending;
    }
    if (m->next_pending != 0) {
        struct qdb__msg *next = qdb__msg_get(st, m->next_pending);
        if (next) { next->prev_pending = m->prev_pending; }
    } else {
        q->pending_tail = m->prev_pending;
    }
    m->next_pending = 0;
    m->prev_pending = 0;
    q->pending_count--;
}

/* -------------------------------------------------------------------------
 * Lease table
 * ---------------------------------------------------------------------- */

struct qdb__lease *qdb__lease_get(struct qdb__state *st, uint64_t lease_id)
{
    uint32_t           b = lease_bucket(lease_id);
    struct qdb__lease *l = st->lease_buckets[b];
    while (l) {
        if (l->lease_id == lease_id) { return l; }
        l = l->next_in_bucket;
    }
    return NULL;
}

int qdb__lease_insert(struct qdb__state *st, struct qdb__lease *l)
{
    uint32_t b            = lease_bucket(l->lease_id);
    l->next_in_bucket     = st->lease_buckets[b];
    st->lease_buckets[b]  = l;
    st->lease_count++;
    return QDB_OK;
}

void qdb__lease_remove(struct qdb__state *st, uint64_t lease_id)
{
    uint32_t            b    = lease_bucket(lease_id);
    struct qdb__lease **pp   = &st->lease_buckets[b];
    struct qdb__lease  *cur  = *pp;

    while (cur) {
        if (cur->lease_id == lease_id) {
            *pp = cur->next_in_bucket;
            free(cur);
            st->lease_count--;
            return;
        }
        pp  = &cur->next_in_bucket;
        cur = cur->next_in_bucket;
    }
}

/* -------------------------------------------------------------------------
 * Iteration
 * ---------------------------------------------------------------------- */

void qdb__state_iter_msgs(struct qdb__state *st, qdb__msg_iter_fn fn, void *ctx)
{
    uint32_t i;
    for (i = 0; i < QDB__MSG_BUCKETS; i++) {
        const struct qdb__msg *m = st->msg_buckets[i];
        while (m) {
            fn(m, ctx);
            m = m->next_in_bucket;
        }
    }
}

void qdb__state_iter_queues(struct qdb__state *st,
                             qdb__queue_iter_fn fn, void *ctx)
{
    uint32_t i;
    for (i = 0; i < QDB__QUEUE_BUCKETS; i++) {
        const struct qdb__queue *q = st->queue_buckets[i];
        while (q) {
            fn(q, ctx);
            q = q->next_in_bucket;
        }
    }
}
