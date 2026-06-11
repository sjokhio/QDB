# QDB

**QDB is SQLite for message queues** — a lightweight, embedded, persistent message queue library written in portable C.

Drop a single library into your application and get a durable, crash-safe queue with no server process, no daemons, and no external dependencies beyond libc.

---

## Why QDB?

Most message queue solutions fall into one of two camps:

- **In-process, in-memory** — fast and simple, but no durability.  Data is lost on crash or restart.
- **Network brokers** (Kafka, RabbitMQ, Redis Streams) — durable, but require running and operating a separate server.

QDB occupies the gap: **embedded durability**. It is to message queues what SQLite is to relational databases — a file-backed engine that lives entirely inside your process.

---

## Design Philosophy

**Reliability first.**  QDB is designed for infrastructure software where data loss is unacceptable.  Every design decision prioritises crash safety over raw throughput.

**No surprises.**  The file format is stable, documented, and forward-compatible.  A queue written by QDB 1.0 will be readable by QDB 2.0.

**Dependency-free.**  QDB links only against libc.  No Boost, no protobuf, no runtime, no package manager.  Drop two files into your project and go.

**Small and auditable.**  The entire implementation targets under 5,000 lines of C.  A careful engineer can read the whole thing in a day.

**Portable.**  QDB targets Linux, macOS, and Windows using only standard POSIX I/O where possible and thin platform shims where not.

---

## Non-Goals (v1)

QDB is deliberately not:

- A distributed system
- A replicated or clustered broker
- A drop-in replacement for Kafka, RabbitMQ, or NATS
- A networked service with an HTTP API
- A multi-tenant system with authentication or access control

If you need those things, a network broker is the right tool.  QDB targets the use-cases where a network broker is operational overkill.

---

## Features

- Persistent, crash-safe queues backed by an append-only log
- Multiple named queues in a single database file
- At-least-once delivery with explicit acknowledgement
- Pure C17, warning-free on GCC, Clang, and MSVC
- No external dependencies beyond libc
- MIT licensed

---

## Building

### Requirements

- CMake 3.20 or later
- A C17-capable compiler (GCC 8+, Clang 7+, MSVC 2019+)

### Quick start

```sh
git clone https://github.com/your-org/qdb.git
cd qdb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

### CMake options

| Option | Default | Description |
|---|---|---|
| `QDB_BUILD_TESTS` | `ON` | Build the test suite |
| `QDB_BUILD_EXAMPLES` | `ON` | Build example programs |
| `QDB_BUILD_BENCHMARKS` | `OFF` | Build benchmark programs |
| `QDB_WARNINGS_AS_ERRORS` | `ON` | Treat compiler warnings as errors |
| `QDB_SANITIZERS` | `OFF` | Enable AddressSanitizer + UBSan |

### Using QDB in your project (CMake FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(
    qdb
    GIT_REPOSITORY https://github.com/your-org/qdb.git
    GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(qdb)

target_link_libraries(my_app PRIVATE qdb::qdb)
```

---

## API Overview

```c
#include <qdb.h>

/* Open (or create) a queue database at the given path. */
qdb_t *db = qdb_open("myapp.qdb");

/* Push a message onto a named queue. */
qdb_push(db, "jobs", "hello world", 11);

/* Pop the next available message from a queue. */
qdb_msg_t msg = {0};
if (qdb_pop(db, "jobs", &msg) == QDB_OK) {
    /* Process msg.data (msg.len bytes) ... */

    /* Acknowledge delivery to remove the message permanently. */
    qdb_ack(db, msg.id);

    /* Release the heap-allocated queue name and data copy. */
    qdb_msg_free(&msg);
}

qdb_close(db);
```

See [include/qdb.h](include/qdb.h) for the full API reference and [examples/](examples/) for runnable programs.

---

## Roadmap

### v1.0 — Foundation
- Append-only storage engine
- Named queues
- Push / pop / ack
- Crash recovery via write-ahead log
- Compaction / log rotation
- Stable on-disk format

### v1.1 — Quality of Life
- Peek without dequeuing
- Message TTL / expiry
- Queue depth query
- Consumer groups (multiple independent consumers on the same queue)

### v1.2 — Performance
- Batch push / batch pop
- Read-ahead buffering
- mmap-backed index

### v2.0 — Extended Semantics
- Delayed delivery
- Priority queues
- Message metadata / headers

---

## Project Status

QDB is under active development.  The API and on-disk format are not yet stable.  See [CHANGELOG.md](CHANGELOG.md) for version history.

---

## Contributing

Contributions are welcome.  Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

---

## Security

To report a vulnerability, please follow the process described in [SECURITY.md](SECURITY.md).

---

## License

QDB is released under the [MIT License](LICENSE).
