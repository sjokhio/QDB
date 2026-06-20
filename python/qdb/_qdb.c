/*
 * _qdb.c — CPython C extension for QDB
 *
 * Exposes qdb.open(), qdb.Database, and qdb.Message.
 *
 * Build strategy: compiled together with the QDB C sources from ../src/.
 * No shared library, no pkg-config. The Python extension embeds libqdb
 * statically via the Extension() source list in setup.py.
 *
 * Thread safety: Database handles must not be shared between threads without
 * external synchronisation (same rule as the C library). The GIL is released
 * around every C call so that other Python threads can run during fsyncs.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "qdb.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Exception objects (module-level singletons)
 * ====================================================================== */

static PyObject *exc_QDBError;
static PyObject *exc_QDBIOError;
static PyObject *exc_QDBCorruptError;
static PyObject *exc_QDBEmptyError;
static PyObject *exc_QDBNotFoundError;
static PyObject *exc_QDBLockedError;

/*
 * Set the appropriate Python exception for a QDB_ERR_* return code.
 * All seven known codes are mapped explicitly. An unexpected negative value
 * falls through to QDBError with the raw code appended to the message.
 * Must be called with the GIL held.
 */
static void
set_qdb_error(int rc)
{
    PyObject *exc;
    switch (rc) {
        case QDB_ERR_IO:     exc = exc_QDBIOError;      break;
        case QDB_ERR_CORRUPT:exc = exc_QDBCorruptError; break;
        case QDB_ERR_INVAL:  exc = exc_QDBError;        break;
        case QDB_ERR_EMPTY:  exc = exc_QDBEmptyError;   break;
        case QDB_ERR_NOENT:  exc = exc_QDBNotFoundError;break;
        case QDB_ERR_NOMEM:  exc = PyExc_MemoryError;   break;
        case QDB_ERR_LOCKED: exc = exc_QDBLockedError;  break;
        default: {
            char buf[80];
            snprintf(buf, sizeof(buf), "%s (code %d)", qdb_errmsg(rc), rc);
            PyErr_SetString(exc_QDBError, buf);
            return;
        }
    }
    PyErr_SetString(exc, qdb_errmsg(rc));
}

/* =========================================================================
 * Message type
 *
 * Immutable snapshot of a dequeued message. All C memory is copied into
 * Python objects (bytes / str) before qdb_msg_free() is called, so no
 * C pointer escapes the pop call.
 * ====================================================================== */

typedef struct {
    PyObject_HEAD
    uint64_t  id;
    uint64_t  lease_id;
    uint32_t  retry_count;
    PyObject *queue;  /* str  — UTF-8 decoded queue name */
    PyObject *data;   /* bytes — raw payload copy        */
} MessageObject;

static PyObject *
Message_get_id(MessageObject *self, void *Py_UNUSED(closure))
{
    return PyLong_FromUnsignedLongLong((unsigned long long)self->id);
}

static PyObject *
Message_get_lease_id(MessageObject *self, void *Py_UNUSED(closure))
{
    return PyLong_FromUnsignedLongLong((unsigned long long)self->lease_id);
}

static PyObject *
Message_get_retry_count(MessageObject *self, void *Py_UNUSED(closure))
{
    return PyLong_FromUnsignedLong((unsigned long)self->retry_count);
}

static PyObject *
Message_get_queue(MessageObject *self, void *Py_UNUSED(closure))
{
    Py_INCREF(self->queue);
    return self->queue;
}

static PyObject *
Message_get_data(MessageObject *self, void *Py_UNUSED(closure))
{
    Py_INCREF(self->data);
    return self->data;
}

/* NULL setters make the attributes read-only — AttributeError on assignment */
static PyGetSetDef Message_getset[] = {
    {"id",          (getter)Message_get_id,          NULL, "monotonic message identifier", NULL},
    {"lease_id",    (getter)Message_get_lease_id,    NULL, "active lease identifier",      NULL},
    {"queue",       (getter)Message_get_queue,       NULL, "source queue name (str)",      NULL},
    {"data",        (getter)Message_get_data,        NULL, "message payload (bytes)",      NULL},
    {"retry_count", (getter)Message_get_retry_count, NULL, "number of prior deliveries",   NULL},
    {NULL}
};

static void
Message_dealloc(MessageObject *self)
{
    Py_XDECREF(self->queue);
    Py_XDECREF(self->data);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
Message_repr(MessageObject *self)
{
    return PyUnicode_FromFormat(
        "qdb.Message(id=%llu, queue=%R, data=%R, lease_id=%llu, retry_count=%lu)",
        (unsigned long long)self->id,
        self->queue,
        self->data,
        (unsigned long long)self->lease_id,
        (unsigned long)self->retry_count
    );
}

static PyTypeObject MessageType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "qdb.Message",
    .tp_basicsize = sizeof(MessageObject),
    .tp_dealloc   = (destructor)Message_dealloc,
    .tp_repr      = (reprfunc)Message_repr,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = PyDoc_STR(
        "Immutable snapshot of a dequeued message.\n\n"
        "Pass to Database.ack() or Database.nack() to resolve the lease.\n"
        "Attributes: id, lease_id, queue (str), data (bytes), retry_count."
    ),
    .tp_getset    = Message_getset,
    .tp_new       = PyType_GenericNew,
};

/*
 * Construct a Message Python object from a populated qdb_msg_t and
 * immediately free the C struct. Returns a new reference, or NULL with
 * an exception set on allocation failure.
 *
 * Ownership: this function always calls qdb_msg_free(msg) before returning,
 * whether or not construction succeeds.
 */
static PyObject *
message_from_c(qdb_msg_t *msg)
{
    MessageObject *obj =
        (MessageObject *)MessageType.tp_alloc(&MessageType, 0);
    if (!obj) {
        qdb_msg_free(msg);
        return NULL;
    }

    obj->id          = msg->id;
    obj->lease_id    = msg->lease_id;
    obj->retry_count = msg->retry_count;

    /* Queue name: decode UTF-8; replace any invalid sequences rather than
     * raising UnicodeDecodeError — queue names are validated at push time. */
    obj->queue = PyUnicode_DecodeUTF8(
        msg->queue, (Py_ssize_t)strlen(msg->queue), "replace");
    if (!obj->queue) {
        qdb_msg_free(msg);
        Py_DECREF(obj);
        return NULL;
    }

    /* Payload: copy into Python bytes. data is NULL when len is zero. */
    obj->data = PyBytes_FromStringAndSize(
        msg->data ? (const char *)msg->data : "",
        (Py_ssize_t)msg->len);
    if (!obj->data) {
        qdb_msg_free(msg);
        Py_DECREF(obj);
        return NULL;
    }

    qdb_msg_free(msg);
    return (PyObject *)obj;
}

/* =========================================================================
 * Database type
 * ====================================================================== */

typedef struct {
    PyObject_HEAD
    qdb_t *handle;  /* NULL after close() */
} DatabaseObject;

/*
 * Verify that the handle is open; set ValueError and return 0 if closed.
 * Returns 1 if open.
 */
static int
db_require_open(DatabaseObject *self)
{
    if (!self->handle) {
        PyErr_SetString(PyExc_ValueError, "database is closed");
        return 0;
    }
    return 1;
}

/*
 * Validate and UTF-8 encode a Python str queue name.
 * Returns a new reference to a bytes object, or NULL with exception set.
 * The caller must Py_DECREF the result.
 */
static PyObject *
encode_queue_name(PyObject *name_obj)
{
    if (!PyUnicode_Check(name_obj)) {
        PyErr_SetString(PyExc_TypeError, "queue name must be str");
        return NULL;
    }
    PyObject *enc = PyUnicode_AsUTF8String(name_obj);
    if (!enc)
        return NULL;

    Py_ssize_t n = PyBytes_GET_SIZE(enc);
    if (n == 0) {
        Py_DECREF(enc);
        PyErr_SetString(PyExc_ValueError, "queue name must not be empty");
        return NULL;
    }
    if (n > QDB_QUEUE_NAME_MAX) {
        Py_DECREF(enc);
        PyErr_Format(PyExc_ValueError,
            "queue name too long (%zd encoded bytes, max %d)",
            (Py_ssize_t)n, QDB_QUEUE_NAME_MAX);
        return NULL;
    }
    return enc;
}

/* -- close() -------------------------------------------------------------- */

static PyObject *
Database_close(DatabaseObject *self, PyObject *Py_UNUSED(args))
{
    if (self->handle) {
        qdb_t *h = self->handle;
        self->handle = NULL;          /* mark closed before C call */
        Py_BEGIN_ALLOW_THREADS
        qdb_close(h);
        Py_END_ALLOW_THREADS
    }
    Py_RETURN_NONE;
}

/* -- context manager ------------------------------------------------------ */

static PyObject *
Database_enter(DatabaseObject *self, PyObject *Py_UNUSED(args))
{
    if (!db_require_open(self))
        return NULL;
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *
Database_exit(DatabaseObject *self, PyObject *args)
{
    /* exc_type, exc_val, exc_tb are ignored; always close.
     * Returning None (falsy) lets any active exception propagate. */
    (void)args;
    return Database_close(self, NULL);
}

/* -- push(queue, payload) ------------------------------------------------- */

static PyObject *
Database_push(DatabaseObject *self, PyObject *args)
{
    PyObject *name_obj;
    Py_buffer buf;

    /* "y*" accepts bytes / bytearray / memoryview; rejects str with TypeError */
    if (!PyArg_ParseTuple(args, "Oy*:push", &name_obj, &buf))
        return NULL;

    PyObject *enc = encode_queue_name(name_obj);
    if (!enc) {
        PyBuffer_Release(&buf);
        return NULL;
    }

    if (!db_require_open(self)) {
        Py_DECREF(enc);
        PyBuffer_Release(&buf);
        return NULL;
    }

    const char *q = PyBytes_AS_STRING(enc);
    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = qdb_push(self->handle, q, buf.buf, (size_t)buf.len);
    Py_END_ALLOW_THREADS

    Py_DECREF(enc);
    PyBuffer_Release(&buf);

    if (rc != QDB_OK) {
        set_qdb_error(rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* -- pop(queue) ----------------------------------------------------------- */

static PyObject *
Database_pop(DatabaseObject *self, PyObject *args)
{
    PyObject *name_obj;
    if (!PyArg_ParseTuple(args, "O:pop", &name_obj))
        return NULL;

    PyObject *enc = encode_queue_name(name_obj);
    if (!enc)
        return NULL;

    if (!db_require_open(self)) {
        Py_DECREF(enc);
        return NULL;
    }

    qdb_msg_t msg = {0};
    const char *q = PyBytes_AS_STRING(enc);
    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = qdb_pop(self->handle, q, &msg);
    Py_END_ALLOW_THREADS

    Py_DECREF(enc);

    if (rc != QDB_OK) {
        set_qdb_error(rc);
        return NULL;
    }

    /* message_from_c takes ownership of msg and calls qdb_msg_free() */
    return message_from_c(&msg);
}

/* -- ack(message) --------------------------------------------------------- */

static PyObject *
Database_ack(DatabaseObject *self, PyObject *args)
{
    MessageObject *msg;
    /* O! requires the argument to be exactly MessageType */
    if (!PyArg_ParseTuple(args, "O!:ack", &MessageType, &msg))
        return NULL;

    if (!db_require_open(self))
        return NULL;

    uint64_t id = msg->id, lease_id = msg->lease_id;
    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = qdb_ack(self->handle, id, lease_id);
    Py_END_ALLOW_THREADS

    if (rc != QDB_OK) {
        set_qdb_error(rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* -- nack(message) -------------------------------------------------------- */

static PyObject *
Database_nack(DatabaseObject *self, PyObject *args)
{
    MessageObject *msg;
    if (!PyArg_ParseTuple(args, "O!:nack", &MessageType, &msg))
        return NULL;

    if (!db_require_open(self))
        return NULL;

    uint64_t id = msg->id, lease_id = msg->lease_id;
    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = qdb_nack(self->handle, id, lease_id);
    Py_END_ALLOW_THREADS

    if (rc != QDB_OK) {
        set_qdb_error(rc);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* -- __del__ -------------------------------------------------------------- */

static void
Database_dealloc(DatabaseObject *self)
{
    if (self->handle) {
        /* Emit ResourceWarning so that forgotten opens are visible in tests
         * and under python -W default. */
        if (PyErr_WarnEx(PyExc_ResourceWarning,
                "qdb.Database was not explicitly closed; "
                "use a 'with' block or call close()", 1) < 0)
            PyErr_Clear();  /* never suppress in dealloc */

        qdb_t *h = self->handle;
        self->handle = NULL;
        qdb_close(h);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/* -- __repr__ ------------------------------------------------------------- */

static PyObject *
Database_repr(DatabaseObject *self)
{
    if (self->handle)
        return PyUnicode_FromString("<qdb.Database open>");
    return PyUnicode_FromString("<qdb.Database closed>");
}

/* -- method table --------------------------------------------------------- */

static PyMethodDef Database_methods[] = {
    {"close",
     (PyCFunction)Database_close,  METH_NOARGS,
     "close() -> None\n\nFlush and close the database. Idempotent."},

    {"__enter__",
     (PyCFunction)Database_enter,  METH_NOARGS,  NULL},

    {"__exit__",
     (PyCFunction)Database_exit,   METH_VARARGS, NULL},

    {"push",
     (PyCFunction)Database_push,   METH_VARARGS,
     "push(queue: str, payload: bytes-like) -> None\n\n"
     "Durably enqueue a message. payload must be bytes, bytearray, or "
     "memoryview (not str)."},

    {"pop",
     (PyCFunction)Database_pop,    METH_VARARGS,
     "pop(queue: str) -> Message\n\n"
     "Dequeue the oldest pending message from queue.\n"
     "Raises QDBEmptyError if the queue has no available messages."},

    {"ack",
     (PyCFunction)Database_ack,    METH_VARARGS,
     "ack(message: Message) -> None\n\n"
     "Permanently remove a leased message."},

    {"nack",
     (PyCFunction)Database_nack,   METH_VARARGS,
     "nack(message: Message) -> None\n\n"
     "Return a leased message to the tail of its queue for redelivery."},

    {NULL}
};

static PyTypeObject DatabaseType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "qdb.Database",
    .tp_basicsize = sizeof(DatabaseObject),
    .tp_dealloc   = (destructor)Database_dealloc,
    .tp_repr      = (reprfunc)Database_repr,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = PyDoc_STR(
        "An open QDB database handle.\n\n"
        "Obtain via qdb.open(). Use as a context manager:\n\n"
        "    with qdb.open('work.qdb') as db:\n"
        "        db.push('q', b'hello')\n\n"
        "Not thread-safe. Use one handle per thread or protect with a Lock."
    ),
    .tp_methods   = Database_methods,
    .tp_new       = PyType_GenericNew,
};

/* =========================================================================
 * Module-level open()
 * ====================================================================== */

static PyObject *
qdb_py_open(PyObject *Py_UNUSED(module), PyObject *args)
{
    const char *path;
    if (!PyArg_ParseTuple(args, "s:open", &path))
        return NULL;

    int err = QDB_OK;
    qdb_t *handle;
    Py_BEGIN_ALLOW_THREADS
    handle = qdb_open_err(path, NULL, &err);
    Py_END_ALLOW_THREADS

    if (!handle) {
        /* err is always set by qdb_open_err when out_err is non-NULL */
        set_qdb_error(err != QDB_OK ? err : QDB_ERR_IO);
        return NULL;
    }

    DatabaseObject *db =
        (DatabaseObject *)DatabaseType.tp_alloc(&DatabaseType, 0);
    if (!db) {
        qdb_close(handle);
        return NULL;
    }
    db->handle = handle;
    return (PyObject *)db;
}

/* =========================================================================
 * Module definition
 * ====================================================================== */

static PyMethodDef module_methods[] = {
    {"open",
     qdb_py_open, METH_VARARGS,
     "open(path: str) -> Database\n\nOpen or create a QDB database."},
    {NULL}
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_qdb",
    "QDB Python bindings — internal extension module.",
    -1,
    module_methods,
};

PyMODINIT_FUNC
PyInit__qdb(void)
{
    PyObject *m = PyModule_Create(&module_def);
    if (!m)
        return NULL;

    /* ---- Exception hierarchy ------------------------------------------ */
    exc_QDBError = PyErr_NewExceptionWithDoc(
        "qdb.QDBError",
        "Base class for all QDB exceptions.",
        PyExc_Exception, NULL);
    if (!exc_QDBError)
        goto error;

    exc_QDBIOError = PyErr_NewExceptionWithDoc(
        "qdb.QDBIOError",
        "Raised on filesystem or flush failure (QDB_ERR_IO).",
        exc_QDBError, NULL);
    if (!exc_QDBIOError)
        goto error;

    exc_QDBCorruptError = PyErr_NewExceptionWithDoc(
        "qdb.QDBCorruptError",
        "Raised when the database file is corrupt or has an unrecognised format.",
        exc_QDBError, NULL);
    if (!exc_QDBCorruptError)
        goto error;

    exc_QDBEmptyError = PyErr_NewExceptionWithDoc(
        "qdb.QDBEmptyError",
        "Raised by pop() when the queue has no available messages.",
        exc_QDBError, NULL);
    if (!exc_QDBEmptyError)
        goto error;

    exc_QDBNotFoundError = PyErr_NewExceptionWithDoc(
        "qdb.QDBNotFoundError",
        "Raised when a queue or message does not exist (QDB_ERR_NOENT).",
        exc_QDBError, NULL);
    if (!exc_QDBNotFoundError)
        goto error;

    exc_QDBLockedError = PyErr_NewExceptionWithDoc(
        "qdb.QDBLockedError",
        "Raised by open() when another process holds the database lock.",
        exc_QDBError, NULL);
    if (!exc_QDBLockedError)
        goto error;

    /* Add exceptions to module (AddObject steals the reference on success,
     * so we INCREF first so we retain our own reference for the static vars) */
    Py_INCREF(exc_QDBError);
    if (PyModule_AddObject(m, "QDBError", exc_QDBError) < 0) {
        Py_DECREF(exc_QDBError);
        goto error;
    }

    Py_INCREF(exc_QDBIOError);
    if (PyModule_AddObject(m, "QDBIOError", exc_QDBIOError) < 0) {
        Py_DECREF(exc_QDBIOError);
        goto error;
    }

    Py_INCREF(exc_QDBCorruptError);
    if (PyModule_AddObject(m, "QDBCorruptError", exc_QDBCorruptError) < 0) {
        Py_DECREF(exc_QDBCorruptError);
        goto error;
    }

    Py_INCREF(exc_QDBEmptyError);
    if (PyModule_AddObject(m, "QDBEmptyError", exc_QDBEmptyError) < 0) {
        Py_DECREF(exc_QDBEmptyError);
        goto error;
    }

    Py_INCREF(exc_QDBNotFoundError);
    if (PyModule_AddObject(m, "QDBNotFoundError", exc_QDBNotFoundError) < 0) {
        Py_DECREF(exc_QDBNotFoundError);
        goto error;
    }

    Py_INCREF(exc_QDBLockedError);
    if (PyModule_AddObject(m, "QDBLockedError", exc_QDBLockedError) < 0) {
        Py_DECREF(exc_QDBLockedError);
        goto error;
    }

    /* ---- Types --------------------------------------------------------- */
    if (PyType_Ready(&DatabaseType) < 0)
        goto error;
    if (PyType_Ready(&MessageType) < 0)
        goto error;

    Py_INCREF((PyObject *)&DatabaseType);
    if (PyModule_AddObject(m, "Database", (PyObject *)&DatabaseType) < 0) {
        Py_DECREF((PyObject *)&DatabaseType);
        goto error;
    }

    Py_INCREF((PyObject *)&MessageType);
    if (PyModule_AddObject(m, "Message", (PyObject *)&MessageType) < 0) {
        Py_DECREF((PyObject *)&MessageType);
        goto error;
    }

    return m;

error:
    Py_XDECREF(exc_QDBError);
    Py_XDECREF(exc_QDBIOError);
    Py_XDECREF(exc_QDBCorruptError);
    Py_XDECREF(exc_QDBEmptyError);
    Py_XDECREF(exc_QDBNotFoundError);
    Py_XDECREF(exc_QDBLockedError);
    Py_DECREF(m);
    return NULL;
}
