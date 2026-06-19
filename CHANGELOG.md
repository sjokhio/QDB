# Changelog

All notable changes to QDB will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
QDB follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added
- `qdb_open_err()`: canonical open function that surfaces the specific
  failure reason through `*out_err`.  `qdb_open()` and `qdb_open_ex()` are
  now thin wrappers around `qdb_open_err()`.  All 14 failure paths in the
  open sequence set a distinct `QDB_ERR_*` code (`QDB_ERR_INVAL`,
  `QDB_ERR_NOMEM`, `QDB_ERR_IO`, `QDB_ERR_LOCKED`, or `QDB_ERR_CORRUPT`)
  before returning NULL.  Both `opts` and `out_err` may be NULL; the
  function is safe regardless.  No storage format change.  10 tests added
  in `tests/test_open_err.c`.  Documented in `docs/api.md`.
- `qdb_pop_any()`: dequeue the globally oldest available message across all
  queues.  Selects the queue whose `pending_head` has the lowest message ID,
  which is the oldest currently-available message in the database (message IDs
  are assigned from a single global counter at push time).  Shares the
  `do_pop()` lease-issuance helper with `qdb_pop()`; writes the same
  `QDB_RT_MSG_LEASE` record; no storage format change.  12 tests added in
  `test_pop_any.c`.  Documented in `docs/api.md`.
- `qdb_queue_list()`: enumerate all queue names into a caller-provided buffer.
  Supports count-only mode (`out=NULL, cap=0`).  No disk I/O, no library
  allocations.  `qdb_queue_name_t` typedef added to `include/qdb.h`.
- `qdb_compact_recommended()`: heuristic helper returning `1` when compaction
  is likely to reclaim significant space
  (`acked_count > 0 && acked_count > pending_count + leased_count`).
  No disk I/O, no allocations.  Documented in `docs/api.md` and
  `docs/maintenance.md`.
- `qdbtool` command-line utility (`tools/qdbtool.c`) built on the public API.
  Commands: `info`, `list`, `stats`, `compact`, `verify`.
  All read commands support `--json` for machine-readable output.
  `compact` supports `--force` (skip recommendation check) and `--dry-run`
  (report without compacting).
  Controlled by the `QDB_BUILD_TOOLS` CMake option (default `ON`).
  Installs to `bin/` alongside the library.
- `docs/maintenance.md`: operational guide covering compaction scheduling,
  backup/restore, monitoring, retry handling, and a startup/shutdown checklist.
- 16 test suites total (added `test_queue_list`, `test_compact_recommended`,
  `test_qdbtool`).

---

## [1.0.0] — 2026-06-13

### Added
- `qdb_open_ex()` and `qdb_open_opts_t` for configurable lease timeout;
  `qdb_open()` remains the zero-configuration entry point.
- `qdb_stats()` and `qdb_stats_t` for database-level observability
  (pending, leased, acked counts; queue count; file size in bytes).
- `qdb_queue_stats()` and `qdb_queue_stats_t` for per-queue observability.
- `qdb_compact()`: crash-safe log compaction via temp-file + atomic rename.
  Excludes ACKed messages from the compacted file; preserves PENDING and
  LEASED messages with original IDs, lease IDs, and expiry timestamps;
  writes a CHECKPOINT record to pin counter monotonicity.
  Stale `-compact` sidecar files from interrupted compactions are cleaned up
  automatically on the next `qdb_open()`.
- Multi-process stress test suite (`test_mp.c`): concurrent push/pop/ack
  workers, lock-contention tests, and sequential hand-off scenarios.
- Crash recovery tests: worker process is killed after `qdb_ack()` and
  after `qdb_nack()` respectively; parent verifies correct queue state after
  reopen.
- Fuzz harnesses for the file header, record parser, and full replay path
  (`fuzz_header`, `fuzz_record_parser`, `fuzz_replay`); 30-second CI smoke
  tests run on every push to `src/` or `include/`.
- API boundary test suite (`test_boundaries.c`): empty-payload round-trip
  (push/pop/ack with `data=NULL, len=0`), 255-byte queue name round-trip,
  255-byte name with `qdb_queue_stats`, 256-byte name rejection at
  push/pop/stats, compact preserving empty-payload messages, compact with
  255-byte queue name.
- Version constants unified: `include/qdb.h` now includes the CMake-generated
  `qdb_version.h` instead of hardcoding `MAJOR/MINOR/PATCH`; `CMakeLists.txt`
  is the single source of truth for the version.  `QDB_VERSION_NUMBER` macro
  added to the generated header.

### Changed
- `examples/hello.c` rewritten from a placeholder stub to a working example
  demonstrating push, pop, ack, stats, and clean shutdown (~90 lines).
- `docs/api.md` expanded with sections for `qdb_open_ex`/`qdb_open_opts_t`,
  `qdb_stats`/`qdb_stats_t`, `qdb_queue_stats`/`qdb_queue_stats_t`, and
  `qdb_compact` including crash-safety contract and recommended usage pattern.
- `docs/mvp-status.md` updated to reflect all shipped features; test suite
  count corrected to 14 suites.
- `docs/benchmarks.md` roadmap table updated: `qdb_stats`, `qdb_queue_stats`,
  and `qdb_compact` marked as implemented.
- README.md overhauled: platform/compiler support matrix added (7 CI
  configurations), stale "planned features" section replaced with accurate
  "Implemented" / "Intentionally absent" split, fuzz build instructions and
  link to `docs/fuzzing.md` added, `examples/hello.c` linked alongside
  `examples/job_worker.c`.

### Fixed
- `next_lease_id` was not persisted across close/reopen when all messages had
  been acknowledged, allowing lease IDs to restart from 1 and potentially
  collide with IDs seen by long-lived callers.  A CHECKPOINT record written
  by `qdb_compact()` and replayed on open now pins both `next_msg_id` and
  `next_lease_id` correctly.
- Windows compaction: `MoveFileExA(MOVEFILE_REPLACE_EXISTING)` fails when
  the destination file has any open handle regardless of `FILE_SHARE_DELETE`.
  `qdb_compact()` now closes `db->fd` before calling `qdb__file_rename()` on
  Windows, then reopens via the normal recovery path.
- `qdb_compact()` post-rename failure: if the internal reopen step fails after
  the database file has already been replaced, the handle is now explicitly
  invalidated (`db->fd` closed, `db->state` freed) so subsequent API calls
  return errors rather than crashing on stale pointers.
- Windows test build: `test_compact.c` was missing the
  `_CRT_SECURE_NO_WARNINGS` define before `<stdio.h>`, causing MSVC C4996
  deprecation errors on `fopen()` under `/W4 /WX`.

---

## [0.1.0] — initial

### Added
- Initial project structure and CMake build system (static library, install
  rules, `FetchContent` support, `qdb::qdb` alias target).
- Public API header (`include/qdb.h`) with fully documented function
  signatures, error codes, and ownership rules.
- Core queue operations: `qdb_open`, `qdb_close`, `qdb_push`, `qdb_pop`,
  `qdb_ack`, `qdb_nack`, `qdb_process_expired_leases`.
- Append-only log storage engine with CRC-32/ISO-HDLC record integrity,
  two-phase durable writes (double-fsync + commit marker), and automatic
  tail-truncation recovery on open.
- Exclusive file lock (`<path>-lock` sidecar) preventing concurrent writers.
- Platform abstraction layer supporting Linux (`fdatasync`), macOS
  (`F_FULLFSYNC`), and Windows (`FlushFileBuffers`, `LockFileEx`).
- GitHub Actions CI: Ubuntu (GCC 12, Clang 15/16/17), macOS (Apple Clang,
  Homebrew Clang), Windows (MSVC x64/Win32, clang-cl); all with
  warnings-as-errors; ASan + UBSan debug builds on Linux and macOS.
- Test suites: storage layer, log replay, push, pop, ack, nack, lease expiry.
- Design documentation: file format spec, storage model, message lifecycle
  state machine, crash recovery protocol, queue semantics, compaction design.
- Six example and benchmark programs.
- Three-harness fuzz infrastructure with corpus generator.

---

[Unreleased]: https://github.com/sjokhio/qdb/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/sjokhio/qdb/compare/v0.1.0...v1.0.0
[0.1.0]: https://github.com/sjokhio/qdb/releases/tag/v0.1.0
