# Multi-Process Stress Test Design

QDB v1 does not support concurrent multi-process access to one database. An
exclusive lock allows only one process to hold a `.qdb` file at a time. These
tests validate that boundary and the reliability of sequential ownership
changes; they do not add multi-process concurrency semantics.
This design is a planning document only; it is not a promise that all scenarios will land in v0.2.0.

## Test model

A controller starts worker executables as separate processes. Workers must not
depend on fork-only behavior so the same scenarios run on Linux, macOS, and
Windows. The controller assigns unique database paths, records operations, and
enforces timeouts.

Workers and the controller communicate through explicit coordination
checkpoints. A worker reports when it has opened the database or completed an
operation, then waits for the controller to continue or terminate it. Crash
tests use these checkpoints instead of sleeps, making termination points
deterministic and reproducible.

## Required scenarios

### Exclusive locking

While one worker holds the database open, all other processes must fail to open
it. This validates lock acquisition and confirms that unsupported concurrent
access is rejected.

### Sequential hand-off

After the owner closes cleanly or is terminated, a new worker must be able to
open the same database. Repeated ownership changes should preserve queue state
and allow only one successful owner at a time.

### Crash recovery

Terminate workers at coordinated checkpoints after push, pop, ack, nack, and
lease acquisition operations. The next worker must reopen the database,
replay it successfully, and observe a valid state. After reopen,
`qdb_process_expired_leases()` must be called explicitly before expecting an
expired leased message to become available again.

### Durability invariants

The controller records operations confirmed with `QDB_OK`. After every crash
and reopen:

- Every confirmed push must remain present unless it was confirmed acked.
- A confirmed acked message must not reappear.
- An unresolved lease may remain leased until expiry is processed.
- Replay must not produce corruption or duplicate committed messages.

## Out of scope

- Concurrent writers
- NFS guarantees
- External deletion of lock files
- Same-process double-open as supported behavior
