# QDB

QDB is a lightweight embedded persistent message queue written in portable C.
Drop `libqdb` into your application and get named, durable, crash-safe queues
backed by an append-only log file — no server process, no daemon, no external
dependencies beyond libc.

Think of it as SQLite for message queues: the entire queue database lives in a
single file on disk.  Your process pushes messages in, pops them out with
time-bounded leases, and acknowledges or negatively-acknowledges each one.  If
your process crashes mid-flight, unacknowledged messages are automatically
recovered and redelivered on the next open.

---

## Why not Kafka, RabbitMQ, or Redis Streams?

Those tools are the right choice when you need distribution, replication, or
multi-tenant access control.  They come with operational weight: separate
processes to manage, network round-trips on every operation, and infrastructure
expertise to keep them running.

QDB fits a different niche — a single application that wants durable queues
without the infrastructure:

| | QDB | Redis / Kafka / RabbitMQ |
|---|---|---|
| Deployment | Single file | Separate server process |
| Dependencies | None (libc only) | Network, runtime, ops infra |
| Multi-process / multi-host | No | Yes |
| Sustained throughput | ~1 000 – 5 000 msg/s | 100 000 + msg/s |
| Durability | Crash-safe, single file | Configurable |
| Operational cost | Zero | Non-trivial |

---

## When to use QDB

- A single-process application (a CLI tool, a daemon, an embedded system)
  wants durable task queues without a broker.
- You need at-least-once delivery with explicit acknowledgement and automatic
  retry on crash.
- Your write workload is hundreds to a few thousand messages per second.
- You want a queue database you can copy, back up, or inspect with standard
  file tools.
- Running a broker is out of scope (resource-constrained device, air-gapped
  host, simple deployment requirement).

## When not to use QDB

- Multiple processes or machines must share the same queue.
- You need pub/sub fan-out, topic routing, or consumer groups.
- Sustained throughput must exceed ~5 000 msg/s on commodity hardware.
- The queue must survive host loss or require geographic replication.
- You need a stable, production-hardened format today (QDB v1 is pre-release).

---

## Building

### Requirements

- CMake 3.20 or later
- A C17-capable compiler: GCC 8+, Clang 7+, or MSVC 2019+

### From source

```sh
git clone https://github.com/your-org/qdb.git
cd qdb
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build          # run the test suite
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
    GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(qdb)

target_link_libraries(my_app PRIVATE qdb::qdb)
```

---

## API overview

```c
#include <qdb.h>

/* ── Lifecycle ─────────────────────────────────────────────────── */

qdb_t *db = qdb_open("myapp.qdb");  /* open or create; NULL on failure */
qdb_close(db);                      /* flush, unlock, free */

/* ── Push ──────────────────────────────────────────────────────── */

qdb_push(db, "jobs", "payload", 7); /* append to named queue */

/* ── Pop → process → acknowledge ──────────────────────────────── */

/* Call before popping to reclaim messages whose leases have expired. */
qdb_process_expired_leases(db);

qdb_msg_t msg = {0};
int rc = qdb_pop(db, "jobs", &msg);

if (rc == QDB_OK) {
    /* msg.data and msg.queue are heap-allocated; owned by the caller. */
    if (process(msg.data, msg.len) == SUCCESS) {
        qdb_ack(db, msg.id, msg.lease_id);   /* remove permanently    */
    } else {
        qdb_nack(db, msg.id, msg.lease_id);  /* return to queue tail  */
    }
    qdb_msg_free(&msg);
} else if (rc == QDB_ERR_EMPTY) {
    /* queue is empty */
}
```

See [docs/api.md](docs/api.md) for the full API reference and ownership rules,
[examples/job_worker.c](examples/job_worker.c) for a complete runnable example,
and [docs/reliability.md](docs/reliability.md) for durability and delivery
guarantees.

---

## Design philosophy

**Reliability first.**  Every design decision prioritises crash safety over raw
throughput.  A push that returns `QDB_OK` is durable: it will survive an
immediate process kill.

**No surprises.**  The file format is stable, documented, and forward-compatible.
A queue written by QDB 0.1 will be readable by QDB 1.0.

**Dependency-free.**  QDB links only against libc.  No Boost, no protobuf, no
runtime, no package manager.  Drop two files into your project.

**Small and auditable.**  The implementation targets under 5 000 lines of C.
A careful engineer can read the whole thing in a day.

**Portable.**  Linux, macOS, and Windows via standard POSIX I/O and thin
platform shims.

---

## Non-goals (v1)

- Distributed or replicated queues
- Multi-process or networked access
- Pub/sub fan-out or topic routing
- Authentication or multi-tenancy
- Throughput-optimised batch paths

---

## Project status

QDB is under active development.  The API and on-disk format are **not yet
stable**.  Do not use in production without understanding the limitations in
[docs/mvp-status.md](docs/mvp-status.md).

See [CHANGELOG.md](CHANGELOG.md) for version history.

---

## Roadmap

### v0.1 — Foundation (current)
- Append-only storage engine with CRC-32 corruption detection
- Named queues with push / pop / ack / nack
- At-least-once delivery with time-bounded leases
- Explicit lease expiry via `qdb_process_expired_leases`
- Crash recovery: full state rebuilt from log on reopen
- File-level exclusive lock (prevents double-open)

### v0.2 — Compaction
- Log compaction / rotation to reclaim disk space
- `qdb_checkpoint()` explicit compaction API

### v0.3 — Observability
- `qdb_queue_depth()` — pending / leased / acked counts
- `qdb_inspect` CLI tool
- `qdb_peek()` — non-consuming read

### v1.0 — Stable
- Stable on-disk format guarantee
- Full Windows CI
- Comprehensive fuzzing

---

## Contributing

Contributions are welcome.  Please read [CONTRIBUTING.md](CONTRIBUTING.md)
before opening a pull request.

## Security

To report a vulnerability, see [SECURITY.md](SECURITY.md).

## License

QDB is released under the [MIT License](LICENSE).
