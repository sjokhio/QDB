# Fuzzing QDB

QDB ships three fuzz harnesses that cover the parser, the log replayer, and
the file-header decoder.  They are designed to be driven by libFuzzer,
AFL++, or run as plain file-replay tools when a fuzzing engine is not
available.

---

## Targets

| Target | What it fuzzes | Input |
|---|---|---|
| `fuzz_record_parser` | `qdb__scan_record` + `qdb__replay_log` | Arbitrary bytes used as the log region; valid header prepended by the harness |
| `fuzz_replay` | `qdb_open` end-to-end | Arbitrary bytes used as a complete database file (header + log) |
| `fuzz_header` | `qdb__header_read` | Up to 4 096 bytes used as the file header; log region is empty |

Running all three in parallel gives the best coverage:

- `fuzz_header` teaches the fuzzer what a valid header looks like, and the
  corpus feeds into `fuzz_replay`.
- `fuzz_record_parser` focuses mutations on record-level structure (type
  bytes, payload lengths, CRC, commit marker) without spending iterations on
  invalid headers.
- `fuzz_replay` covers interactions between header-field values and log
  content that neither other target can reach alone.

---

## Quick start — libFuzzer (Clang)

```sh
# 1. Configure with ASan + UBSan for maximum bug detection.
#    Build the library with sanitizers so bugs inside qdb.c are found too.
cmake -B build-fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DQDB_BUILD_FUZZ=ON \
  -DQDB_BUILD_TESTS=OFF \
  -DQDB_SANITIZERS=ON \
  -DQDB_WARNINGS_AS_ERRORS=OFF

cmake --build build-fuzz --parallel \
  --target fuzz_record_parser fuzz_replay fuzz_header gen_seeds

# 2. Generate an initial seed corpus from real QDB databases.
mkdir -p fuzz/corpus
build-fuzz/fuzz/gen_seeds fuzz/corpus

# 3. Run each target.  Use separate output directories per target.
mkdir -p findings/record_parser findings/replay findings/header

build-fuzz/fuzz/fuzz_record_parser \
  -dict=fuzz/qdb.dict \
  -artifact_prefix=findings/record_parser/ \
  fuzz/corpus/record_parser &

build-fuzz/fuzz/fuzz_replay \
  -dict=fuzz/qdb.dict \
  -artifact_prefix=findings/replay/ \
  fuzz/corpus/replay &

build-fuzz/fuzz/fuzz_header \
  -dict=fuzz/qdb.dict \
  -artifact_prefix=findings/header/ \
  fuzz/corpus/header &

wait
```

### Useful libFuzzer flags

| Flag | Effect |
|---|---|
| `-max_total_time=N` | Stop after N seconds |
| `-jobs=N -workers=N` | Run N parallel instances |
| `-max_len=N` | Cap input size to N bytes (default: 4 096 for header, 131 072 for others) |
| `-timeout=N` | Kill a single run after N seconds (default: 1 200) |
| `-print_final_stats=1` | Print coverage stats on exit |
| `-use_value_profile=1` | Track value-coverage for deeper mutations |

---

## AFL++

AFL++ supports two modes for these harnesses.

### Mode 1 — afl-clang-fast (recommended)

`afl-clang-fast` is Clang-based and supports the `LLVMFuzzerTestOneInput`
interface natively.  Build the same way as libFuzzer:

```sh
cmake -B build-afl \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=afl-clang-fast \
  -DQDB_BUILD_FUZZ=ON \
  -DQDB_BUILD_TESTS=OFF \
  -DQDB_WARNINGS_AS_ERRORS=OFF

cmake --build build-afl --parallel \
  --target fuzz_record_parser fuzz_replay fuzz_header gen_seeds

build-afl/fuzz/gen_seeds fuzz/corpus

afl-fuzz -i fuzz/corpus/replay \
         -o findings/afl-replay \
         -x fuzz/qdb.dict \
         -- build-afl/fuzz/fuzz_replay
```

### Mode 2 — afl-gcc-fast (standalone harness + `@@`)

If you are using `afl-gcc-fast` or cannot use a Clang-based compiler, build
the harnesses in standalone mode.  AFL++ passes the input file path via `@@`.

```sh
cmake -B build-afl-gcc \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=afl-gcc-fast \
  -DQDB_BUILD_FUZZ=ON \
  -DQDB_FUZZ_STANDALONE=ON \
  -DQDB_BUILD_TESTS=OFF \
  -DQDB_WARNINGS_AS_ERRORS=OFF

cmake --build build-afl-gcc --parallel \
  --target fuzz_record_parser fuzz_replay fuzz_header gen_seeds

build-afl-gcc/fuzz/gen_seeds fuzz/corpus

# The standalone main reads the file given as its first argument.
afl-fuzz -i fuzz/corpus/replay \
         -o findings/afl-gcc-replay \
         -x fuzz/qdb.dict \
         -- build-afl-gcc/fuzz/fuzz_replay @@
```

---

## Standalone replay (no fuzzing engine)

The standalone harness is useful for:

- Verifying that a specific crash input actually reproduces.
- Testing seeds on any compiler (GCC, MSVC).
- Running a quick sanity check in environments where Clang is unavailable.

```sh
cmake -B build-standalone \
  -DQDB_BUILD_FUZZ=ON \
  -DQDB_FUZZ_STANDALONE=ON

cmake --build build-standalone --parallel \
  --target fuzz_record_parser fuzz_replay fuzz_header

# Pass one or more corpus files as arguments.
build-standalone/fuzz/fuzz_replay fuzz/corpus/replay/*.qdb

# Reproduce a specific crash:
build-standalone/fuzz/fuzz_replay crash-abc123
```

---

## Seed corpus

Good seeds dramatically improve fuzzer efficiency.  The `gen_seeds` utility
builds minimal, valid QDB databases that cover every record type:

```sh
build-fuzz/fuzz/gen_seeds fuzz/corpus
```

This writes files into three sub-directories:

```
fuzz/corpus/
  record_parser/   — log regions (for fuzz_record_parser)
  replay/          — complete .qdb files (for fuzz_replay)
  header/          — 4 096-byte headers (for fuzz_header)
```

You can also copy database files produced by the test suite:

```sh
cmake --build build --target test_push test_nack test_expire
# Run the tests to produce .qdb files in the test working directory
(cd build/tests && ./test_push && ./test_nack && ./test_expire)
cp build/tests/*.qdb fuzz/corpus/replay/
```

---

## Dictionary

`fuzz/qdb.dict` contains magic bytes, record type bytes, fixed-size payload
lengths, and interesting boundary values.  Pass it to any fuzzer:

```sh
# libFuzzer
-dict=fuzz/qdb.dict

# AFL++
-x fuzz/qdb.dict
```

---

## CI smoke test

The `.github/workflows/fuzz.yml` workflow runs each harness for 30 seconds
on every push to `main` that touches `src/`, `include/`, or `fuzz/`.  This
is a regression gate, not a full campaign.

To interpret workflow results:
- **Exit 0** — no crash found within the time limit.
- **Exit non-zero** — a crash, ASan finding, or UBSan finding was
  detected.  Crash inputs are uploaded as workflow artifacts named
  `fuzz-crashes`.

---

## Running a longer campaign

For real bug-finding, run for hours or days with multiple parallel jobs:

```sh
# 4 parallel libFuzzer instances, no time limit
build-fuzz/fuzz/fuzz_replay \
  -dict=fuzz/qdb.dict \
  -jobs=4 \
  -workers=4 \
  -artifact_prefix=findings/replay/ \
  fuzz/corpus/replay
```

Or with AFL++ and multiple cores:

```sh
# Primary instance
afl-fuzz -M primary \
  -i fuzz/corpus/replay -o findings/afl \
  -x fuzz/qdb.dict \
  -- build-afl/fuzz/fuzz_replay &

# Secondary instances (as many cores as available)
for i in 1 2 3; do
  afl-fuzz -S "worker$i" \
    -i fuzz/corpus/replay -o findings/afl \
    -x fuzz/qdb.dict \
    -- build-afl/fuzz/fuzz_replay &
done
```

Check coverage after a campaign:

```sh
# libFuzzer coverage report (requires llvm-cov)
cmake -B build-cov \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DQDB_BUILD_FUZZ=ON \
  -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
  -DQDB_WARNINGS_AS_ERRORS=OFF
cmake --build build-cov --parallel --target fuzz_replay

LLVM_PROFILE_FILE="fuzz.profraw" \
  build-cov/fuzz/fuzz_replay -runs=0 fuzz/corpus/replay

llvm-profdata merge -o fuzz.profdata fuzz.profraw
llvm-cov show build-cov/fuzz/fuzz_replay \
  -instr-profile=fuzz.profdata \
  -format=html -output-dir=coverage-report
```

---

## Interpreting a crash

When a fuzzer finds a crash:

1. The input is saved to a file named `crash-<hash>` (libFuzzer) or
   `findings/afl/default/crashes/id:<n>,...` (AFL++).

2. Reproduce with the standalone harness:
   ```sh
   build-standalone/fuzz/fuzz_replay crash-abc123
   ```

3. For a symbolised stack trace, rebuild with debug symbols and reproduce:
   ```sh
   cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug \
     -DQDB_BUILD_FUZZ=ON -DQDB_FUZZ_STANDALONE=ON -DQDB_SANITIZERS=ON
   cmake --build build-debug --target fuzz_replay
   build-debug/fuzz/fuzz_replay crash-abc123
   ```

4. Open an issue with:
   - The crash input (attach the file).
   - The sanitizer output.
   - The compiler version and OS.

---

## What the harnesses do NOT test

- Multi-threaded access (QDB is single-threaded per handle; the harnesses
  are single-threaded too).
- `qdb_push` / `qdb_ack` / `qdb_nack` with arbitrary inputs — these
  functions validate their arguments before touching any file, so the
  interesting bugs are in the read path.  The `fuzz_replay` harness does
  call `qdb_push` and `qdb_ack` after a successful open to stress the
  combined read+write path.
- File-system exhaustion (out-of-disk-space handling).
