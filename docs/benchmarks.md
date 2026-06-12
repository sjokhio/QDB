# QDB Benchmark Baseline

This document records the initial performance baseline for the v0.1 MVP.
All numbers were captured on a single machine in a single session; they are
reference points for tracking regressions, not marketing claims.

---

## Environment

| Field | Value |
|---|---|
| Date | 2026-06-11 |
| Host | Apple M4, 16 GiB RAM |
| OS | macOS 26.5.1 (Darwin 25.5.0 arm64) |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.3.1 |
| Build type | Release (`-O2`, no sanitizers) |
| Storage | Internal NVMe (Apple) |
| fsync primitive | `fcntl(fd, F_FULLFSYNC)` |

> **macOS note:** `F_FULLFSYNC` issues a hardware-level flush command to the
> drive — equivalent to `fdatasync()` + a drive write-back cache flush.  It
> is significantly slower than Linux `fdatasync()` on spinning disk but
> provides stronger durability guarantees.  Linux numbers will differ; see
> [Platform differences](#platform-differences) below.

---

## Benchmark: `bench_push_pop`

Measures single-threaded, single-message throughput for the two hot paths.
Each operation is fully durable before the call returns.

**Payload:** 16-byte string literal (`"hello from bench"`).  
**Queue:** single queue named `"bench"`.  
**Phase 1:** Push N messages sequentially; measure wall time.  
**Phase 2:** Pop + ack each message in FIFO order; measure wall time.

### Release build results

```
$ ./build-release/benchmarks/bench_push_pop 1000

QDB push/pop+ack benchmark  (N=1000, payload=16 bytes)

push        1000 msgs  11.629 s        86 msg/s
pop+ack     1000 msgs  23.596 s        42 msg/s
total       2000 msgs  35.225 s        57 msg/s
```

### Stability across message counts (debug build)

| N | push (msg/s) | pop+ack (pairs/s) | total (ops/s) |
|---|---|---|---|
| 100 | 77 | 31 | 44 |
| 500 | 85 | 41 | 55 |
| 1 000 | 82 | 38 | 52 |

The rate is flat across all N values, confirming that throughput is
bounded by per-operation latency, not by any amortisable overhead such
as buffer allocation or file-system metadata.

Release vs debug difference is ~5 % — negligible, confirming the CPU
component of each operation is not the bottleneck.

---

## Why these numbers

Every durable operation in QDB calls `qdb__append_record` followed by
`qdb__header_update`.  Each of these calls `F_FULLFSYNC` at least once:

| Operation | fsyncs |
|---|---|
| `qdb_push` | 3 (payload flush + commit-marker flush + header flush) |
| `qdb_pop` | 3 (lease-record flush × 2 + header flush) |
| `qdb_ack` | 3 (ack-record flush × 2 + header flush) |
| **pop + ack pair** | **6** |

Measured `F_FULLFSYNC` latency derived from the benchmark numbers:

| Operation | wall time / op | fsyncs / op | implied µs / fsync |
|---|---|---|---|
| push | 11.6 ms | 3 | ~3 900 µs |
| pop+ack | 23.6 ms | 6 | ~3 900 µs |

At ~3.9 ms per hardware flush, the Apple M4's NVMe controller is the
rate-limiting resource.  Reducing the number of `fsync` calls per
logical operation (group commit) is the primary lever for throughput
improvement; see [Roadmap](#roadmap-impact).

---

## Platform differences

| Platform | fsync primitive | Typical latency | Expected push throughput |
|---|---|---|---|
| macOS (Apple SSD) | `F_FULLFSYNC` | 3–6 ms | 60–100 msg/s |
| macOS (external SSD) | `F_FULLFSYNC` | 5–20 ms | 15–60 msg/s |
| Linux (NVMe, `fdatasync`) | `fdatasync` | 0.05–0.5 ms | 700–6 000 msg/s |
| Linux (SATA SSD, `fdatasync`) | `fdatasync` | 0.1–2 ms | 400–3 000 msg/s |
| Linux (tmpfs / `fdatasync`) | no-op | < 0.01 ms | > 30 000 msg/s |

The Linux estimates assume the drive write-back cache is enabled (the
common default).  With `hdparm -W 0` (write cache disabled) Linux numbers
approach the macOS `F_FULLFSYNC` range.

---

## How to reproduce

```sh
# 1. Configure (Release, benchmarks enabled)
cmake -DQDB_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release -S . -B build-release

# 2. Build
cmake --build build-release --target bench_push_pop

# 3. Run
./build-release/benchmarks/bench_push_pop          # N=1000 (default)
./build-release/benchmarks/bench_push_pop 5000     # custom N
```

The benchmark writes and deletes `bench_push_pop.qdb` (and sidecar files)
in the current working directory.  Run from a directory on the target
storage device to measure the intended medium.

---

## Roadmap impact

| Planned feature | Expected effect |
|---|---|
| Group commit (batch fsync) | 10–100× throughput improvement; reduces fsyncs per message to O(1/batch) |
| WAL write path | Amortises header updates; removes one fsync per operation |
| Queue-depth API | No throughput impact |
| Compaction | No throughput impact on write path |

The current per-message fsync model is intentional for the v0.1 MVP:
it is simple, correct, and easy to reason about.  Throughput improvement
is explicitly listed as a v0.2+ goal in the [project roadmap](../README.md).
