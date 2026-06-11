/*
 * qdb.c — QDB implementation
 *
 * This file will contain the full implementation of the QDB embedded
 * message queue engine.  At present it provides only the scaffolding:
 * stub implementations that satisfy the linker so the project builds,
 * and the two trivial utility functions (qdb_errmsg, qdb_version) that
 * have no dependencies on storage internals.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb.h"

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Internal structure (forward declaration placeholder)
 *
 * The real definition will live here once storage is implemented.
 * ---------------------------------------------------------------------- */

struct qdb {
    int placeholder; /* TODO: replace with real fields */
};

/* -------------------------------------------------------------------------
 * Lifecycle — stubs
 * ---------------------------------------------------------------------- */

qdb_t *qdb_open(const char *path)
{
    (void)path;
    /* TODO: implement open / create / recovery */
    return NULL;
}

void qdb_close(qdb_t *db)
{
    (void)db;
    /* TODO: implement flush and resource release */
}

/* -------------------------------------------------------------------------
 * Queue operations — stubs
 * ---------------------------------------------------------------------- */

int qdb_push(qdb_t *db, const char *queue, const void *data, size_t len)
{
    (void)db;
    (void)queue;
    (void)data;
    (void)len;
    /* TODO: implement append-only log write */
    return QDB_ERR_IO;
}

int qdb_pop(qdb_t *db, const char *queue, qdb_msg_t *msg)
{
    (void)db;
    (void)queue;
    (void)msg;
    /* TODO: implement dequeue with pending-ack state */
    return QDB_ERR_EMPTY;
}

int qdb_ack(qdb_t *db, uint64_t msg_id)
{
    (void)db;
    (void)msg_id;
    /* TODO: implement acknowledgement and durable deletion */
    return QDB_ERR_NOENT;
}

/* -------------------------------------------------------------------------
 * Utilities
 * ---------------------------------------------------------------------- */

const char *qdb_errmsg(int err)
{
    switch (err) {
    case QDB_OK:          return "success";
    case QDB_ERR_IO:      return "I/O error";
    case QDB_ERR_CORRUPT: return "database corrupt or unrecognised format";
    case QDB_ERR_INVAL:   return "invalid argument";
    case QDB_ERR_EMPTY:   return "queue is empty";
    case QDB_ERR_NOENT:   return "message ID not found";
    case QDB_ERR_NOMEM:   return "out of memory";
    case QDB_ERR_LOCKED:  return "database locked by another process";
    default:              return "unknown error";
    }
}

const char *qdb_version(void)
{
    return "0.1.0";
}
