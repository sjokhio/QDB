# QDB

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C17](https://img.shields.io/badge/C-17-blue.svg)](#building)
[![Linux](https://img.shields.io/badge/Linux-supported-brightgreen.svg)](#platform-support)
[![macOS](https://img.shields.io/badge/macOS-supported-brightgreen.svg)](#platform-support)
[![Windows](https://img.shields.io/badge/Windows-supported-brightgreen.svg)](#platform-support)
[![Linux CI](https://github.com/sjokhio/qdb/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/sjokhio/qdb/actions/workflows/ci-linux.yml)
[![macOS CI](https://github.com/sjokhio/qdb/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/sjokhio/qdb/actions/workflows/ci-macos.yml)
[![Windows CI](https://github.com/sjokhio/qdb/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/sjokhio/qdb/actions/workflows/ci-windows.yml)

---

**QDB is SQLite for message queues.**

An embedded durable queue library written in portable C17.  
No server. No daemon. No external dependencies.  
Just link a library and start queuing jobs.

---

---

## Contents

- [30-second quickstart](#30-second-quickstart)
- [Why QDB?](#why-qdb)
- [When to use QDB](#when-to-use-qdb)
- [When NOT to use QDB](#when-not-to-use-qdb)
- [Performance](#performance)
- [Building](#building)
- [API overview](#api-overview)
- [CLI tool (qdbtool)](#cli-tool-qdbtool)
- [Design philosophy](#design-philosophy)
- [Status](#status)
- [Contributing](#contributing)

---

## 30-second quickstart

```c
#include <qdb.h>
#include <stdio.h>

int main(void)
{
    /* Open (or create) the queue database: one file on disk. */
    qdb_t *db = qdb_open("myapp.qdb");
    if (!db) { fputs("qdb_open failed\n", stderr); return 1; }

    /* Push a job onto the "tasks" queue. Durable before this returns. */
    qdb_push(db, "tasks", "resize-image-42.jpg", 19);

    /* At the top of your worker loop: reclaim messages whose leases expired. */
    qdb_process_expired_leases(db);

    /* Pop the oldest message. It is now exclusively leased to this caller. */
    qdb_msg_t msg = {0};
    if (qdb_pop(db, "tasks", &msg) == QDB_OK) {
        printf("processing: %.*s\n", (int)msg.len, (char *)msg.data);

        /* Acknowledge success: message removed permanently. */
        qdb_ack(db, msg.id, msg.lease_id);

        /* Or on failure: return to queue tail for retry. */
        /* qdb_nack(db, msg.id, msg.lease_id); */

        qdb_msg_free(&msg);
    }

    qdb_close(db);
    return 0;
}
```

**Compile:**

```sh
# after building and installing QDB (see Building below)
cc -o worker worker.c -lqdb
```

Full working examples:

- [`examples/hello.c`](examples/hello.c) — minimal push/pop/ack/stats in ~90 lines
- [`examples/job_worker.c`](examples/job_worker.c) — worker loop with NACK and retry simulation

Complete API reference: [`docs/api.md`](docs/api.md)

---

## Why QDB?

| | |
|---|---|
| **Embedded** | Runs in your process. No server to start, monitor, or restart. |
| **Durable** | Every `qdb_push` and `qdb_ack` is fsynced to disk before returning `QDB_OK`. An immediate `kill -9` will not lose committed messages. |
| **Crash recovery** | On `qdb_open`, QDB replays the append-only log and fully reconstructs queue state, requiring no manual repair step. |
| **At-least-once delivery** | Messages are exclusively leased on pop. If your process crashes before acking, the lease expires and the message is automatically redelivered. |
| **Compaction** | `qdb_compact()` rewrites the log to contain only live messages. Crash-safe: a crash before the atomic rename leaves the original intact. |
| **Observable** | `qdb_stats()` and `qdb_queue_stats()` return pending/leased/acked counts without disk I/O. |
| **Small footprint** | Under 5 000 lines of C17. Zero dependencies beyond libc. The entire implementation is auditable in a day. |
| **Cross-platform** | Linux, macOS, and Windows via thin platform shims over POSIX and Win32 file APIs. |

---

## When to use QDB

- A **single process** (CLI tool, background daemon, embedded device) needs
  durable task queues without deploying a broker.
- You need **at-least-once delivery** with explicit acknowledgement and
  automatic retry on crash or timeout.
- Your write workload is in the range of **tens to a few hundred messages
  per second** and you value durability over raw throughput.
- You want a queue store you can **copy, back up, or inspect** with standard
  file tools, with no proprietary formats or admin clients.
- Running a message broker is out of scope: resource-constrained device,
  air-gapped host, or zero-ops deployment requirement.

---

## When NOT to use QDB

- **Multiple processes or machines** must share the same queue: QDB holds
  an exclusive file lock, so only one process can open a database at a time.
- You need **pub/sub fan-out**, topic routing, or consumer groups.
- **Sustained throughput must exceed ~5 000 msg/s**: the fsync-per-message
  model intentionally prioritises durability; see [Performance](#performance).
- The queue must survive **host loss** or require geographic replication.
- You need multi-tenant access control, authentication, or encryption at rest.

---

## Performance

Measured on Apple M4 (macOS, `F_FULLFSYNC`), Release build, 16-byte payload:

| Operation | Throughput | fsyncs / op |
|---|---|---|
| `qdb_push` | **~86 msg/s** | 3 |
| `qdb_pop` + `qdb_ack` | **~42 pairs/s** | 6 |

Throughput is bounded entirely by hardware-flush latency (~3.9 ms per
`F_FULLFSYNC` on the test machine), not by CPU or memory. The rate is flat
across all message counts tested (N = 100 to 1 000).

**Linux numbers will be significantly higher.** On NVMe with `fdatasync`
(typical default, write cache enabled) expect 700–6 000 msg/s for push.

> Full methodology, platform comparison table, and per-fsync latency
> derivation: [`docs/benchmarks.md`](docs/benchmarks.md)

The primary future throughput lever is group commit / batch fsync, which can
reduce the number of storage flushes required per operation.

---

## Building

### Requirements

- CMake 3.20 or later
- A C17-capable compiler (see [Platform support](#platform-support) for tested versions)

### From source

```sh
git clone https://github.com/sjokhio/qdb.git
cd qdb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure   # 16 suites
```

To run with sanitizers (Linux / macOS, Clang or GCC):

```sh
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DQDB_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

To build and run the fuzz harnesses (libFuzzer / AFL++, requires Clang):

```sh
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang \
      -DQDB_BUILD_FUZZ=ON -DQDB_SANITIZERS=ON
cmake --build build-fuzz --parallel
```

See [`docs/fuzzing.md`](docs/fuzzing.md) for full instructions including AFL++,
corpus setup, CI interpretation, and crash reproduction.

### CMake options

| Option | Default | Description |
|---|---|---|
| `QDB_BUILD_TESTS` | `ON` | Build the test suite |
| `QDB_BUILD_EXAMPLES` | `ON` | Build example programs |
| `QDB_BUILD_BENCHMARKS` | `OFF` | Build benchmark programs |
| `QDB_BUILD_FUZZ` | `OFF` | Build libFuzzer / AFL++ harnesses (Clang) |
| `QDB_WARNINGS_AS_ERRORS` | `ON` | Treat compiler warnings as errors |
| `QDB_SANITIZERS` | `OFF` | Enable AddressSanitizer + UBSan |

### Add to your project (CMake FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(
    qdb
    GIT_REPOSITORY https://github.com/sjokhio/qdb.git
    GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(qdb)

target_link_libraries(my_app PRIVATE qdb::qdb)
```

### Platform support

Compilers and OS versions tested in CI on every push:

| Platform | OS | Compiler | Build types | fsync primitive |
|---|---|---|---|---|
| Linux | Ubuntu 22.04 | GCC 12 | Debug, Release, ASan+UBSan | `fdatasync` |
| Linux | Ubuntu 22.04 | Clang 15, 16, 17 | Debug, Release, ASan+UBSan | `fdatasync` |
| macOS | macOS 14 (M-series) | Apple Clang (Xcode) | Debug, Release, ASan+UBSan | `F_FULLFSYNC` |
| macOS | macOS 14 (M-series) | Homebrew LLVM | Debug, Release | `F_FULLFSYNC` |
| Windows | Windows Server 2022 | MSVC (x64) | Debug, Release | `FlushFileBuffers` |
| Windows | Windows Server 2022 | MSVC (Win32) | Debug, Release | `FlushFileBuffers` |
| Windows | Windows Server 2022 | clang-cl | Debug, Release | `FlushFileBuffers` |

All configurations run with warnings-as-errors. Linux and macOS debug builds
additionally run the full test suite under AddressSanitizer and UBSan.

**Minimum compiler versions** (not CI-tested, but expected to work):
GCC 8+, Clang 7+, MSVC 2019+ (requires `/std:c17`).

---

## API overview

```c
/* Lifecycle */

qdb_t *db = qdb_open("myapp.qdb");   /* open or create; NULL on failure   */
qdb_close(db);                        /* flush, unlock, free               */

/* Configurable lease timeout (default: 30 s) */

qdb_open_opts_t opts = {0};
opts.lease_timeout_s = 120;
qdb_t *db = qdb_open_ex("myapp.qdb", &opts);

/* Push */

int rc = qdb_push(db, "jobs", payload, len);   /* append to named queue   */

/* Pop, process, resolve */

qdb_process_expired_leases(db);   /* call at top of worker loop           */

qdb_msg_t msg = {0};
rc = qdb_pop(db, "jobs", &msg);   /* dequeue from named queue             */

/* Or dequeue the globally oldest message across all queues */
rc = qdb_pop_any(db, &msg);       /* msg.queue tells you where it came from */

if (rc == QDB_OK) {
    /* msg.data and msg.queue are heap-allocated copies; caller owns them */
    if (process(msg.data, msg.len) == SUCCESS) {
        qdb_ack(db,  msg.id, msg.lease_id);   /* remove permanently       */
    } else {
        qdb_nack(db, msg.id, msg.lease_id);   /* return to queue tail     */
    }
    qdb_msg_free(&msg);
} else if (rc == QDB_ERR_EMPTY) {
    /* no available messages (queue or database is empty) */
}

/* Stats */

qdb_stats_t db_stats = {0};
qdb_stats(db, &db_stats);                         /* database-level counts */

qdb_queue_stats_t qs = {0};
qdb_queue_stats(db, "jobs", &qs);                 /* per-queue counts      */

/* Compaction: rewrite log to drop acked records */

qdb_process_expired_leases(db);   /* expire stale leases first            */
qdb_compact(db);

/* Error handling */

fprintf(stderr, "%s\n", qdb_errmsg(rc));   /* human-readable description  */
```

**Key error codes:**

| Code | Meaning |
|---|---|
| `QDB_OK` | Success |
| `QDB_ERR_EMPTY` | Queue has no available (PENDING) messages |
| `QDB_ERR_INVAL` | Invalid argument or wrong `lease_id` |
| `QDB_ERR_NOENT` | Message not found, not leased, or already ACKed |
| `QDB_ERR_CORRUPT` | Database file is corrupt or unrecognised |
| `QDB_ERR_LOCKED` | Another process has this database open |

Full reference: [`docs/api.md`](docs/api.md) · Durability guarantees: [`docs/reliability.md`](docs/reliability.md) · Operational guide: [`docs/maintenance.md`](docs/maintenance.md)

---

## CLI tool (qdbtool)

`qdbtool` is a command-line utility for inspecting and maintaining QDB
database files. It is built entirely on the public QDB API.

```sh
qdbtool info    myapp.qdb              # database summary
qdbtool list    myapp.qdb              # queue names
qdbtool stats   myapp.qdb [<queue>]    # per-queue counts
qdbtool compact [--force] [--dry-run] myapp.qdb
qdbtool verify  myapp.qdb              # open-time integrity check
```

All read commands accept `--json` for machine-readable output:

```sh
qdbtool info --json myapp.qdb
# {"file":"myapp.qdb","size_bytes":4206,"queues":2,"pending":5,
#  "leased":0,"acked":12,"compact_advised":true}

qdbtool list --json myapp.qdb
# ["emails","jobs"]

qdbtool stats --json myapp.qdb
# [{"queue":"emails","pending":3,"leased":0,"acked":8},
#  {"queue":"jobs","pending":2,"leased":0,"acked":4}]
```

**Note:** `qdbtool` acquires the same exclusive file lock as `qdb_open()`.
If another process has the database open, every command fails immediately.
Stop the application before running `qdbtool`.

Built by default (`QDB_BUILD_TOOLS=ON`). Installs to `bin/` alongside the
library. Run `qdbtool --help` for the full flag reference.

---

## Design philosophy

**Reliability first.** Every design decision prioritises crash safety over raw
throughput. A push that returns `QDB_OK` is durable: it will survive an
immediate process kill.

**No surprises.** The append-only file format is documented, human-inspectable,
and designed for forward compatibility. The goal is that a file written today
remains readable by future versions.

**Dependency-free.** QDB links only against libc. No Boost, no protobuf, no
runtime, no package manager. Drop two files into your project.

**Small and auditable.** The implementation targets under 5 000 lines of C17.
A careful engineer can read the whole thing in a day.

**Portable.** Linux, macOS, and Windows via thin platform shims over POSIX
and Win32 file APIs, with no `#ifdef` spaghetti in the core logic.

---

## Status

QDB v1.0.x is the current stable release. The embedded, durable, single-file 
core design is complete and stable. Active development is focused on incremental v1.1 improvements. See
[`docs/mvp-status.md`](docs/mvp-status.md) for a detailed feature breakdown
and [`CHANGELOG.md`](CHANGELOG.md) for change history.

**Implemented and tested:**

- Durable push / pop / ack / nack with at-least-once delivery
- Configurable lease timeout (`qdb_open_ex`)
- Automatic crash recovery on open (log replay)
- Exclusive file lock (single-writer enforcement)
- `qdb_compact()`: crash-safe log compaction via atomic rename
- `qdb_stats()` / `qdb_queue_stats()` / `qdb_queue_list()`: in-memory observability and queue enumeration
- 16 test suites including multi-process and crash recovery scenarios
- Fuzz harnesses for the header, record parser, and full replay path
- CI on Linux (GCC 12, Clang 15/16/17), macOS 14, and Windows (MSVC, clang-cl)
- Operational guide: [`docs/maintenance.md`](docs/maintenance.md)

**Intentionally absent (v1.0 non-goals):**

- Multi-process shared access without external coordination
- Networked/distributed access
- Pub/sub fan-out, topic routing, or consumer groups
- Batch push / group commit (fsync cost dominates at single-message granularity)
- Background lease expiry thread
- Built-in dead-letter queue policy
- Built-in retry limit policy
- Encryption at rest, authentication, or access control

---

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md)
before opening a pull request.

## Security

To report a vulnerability, see [SECURITY.md](SECURITY.md).

## License

QDB is released under the [MIT License](LICENSE).
