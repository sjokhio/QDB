# Contributing to QDB

Thank you for your interest in contributing.  QDB aims to be a production-grade infrastructure library, and contributions are held to a correspondingly high standard.  This document explains the process, conventions, and expectations.

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Coding Standards](#coding-standards)
3. [Commit Message Conventions](#commit-message-conventions)
4. [Pull Request Process](#pull-request-process)
5. [Testing Requirements](#testing-requirements)
6. [Reporting Bugs](#reporting-bugs)
7. [Suggesting Features](#suggesting-features)

---

## Getting Started

1. Fork the repository and clone your fork.
2. Create a feature branch from `main`: `git checkout -b feat/your-feature`.
3. Make your changes following the standards below.
4. Ensure the full test suite passes: `ctest --test-dir build`.
5. Open a pull request against `main`.

For anything beyond a small bug fix, please open an issue first to discuss the approach.  This avoids wasted effort if a proposal doesn't align with the project's direction.

---

## Coding Standards

QDB is written in **C17**.  All code must compile warning-free under GCC, Clang, and MSVC with `-Wall -Wextra -Wpedantic` (or equivalent) and warnings treated as errors.

### Language and dialect

- C17 (`-std=c17`) only.  No compiler extensions.
- No C++.  No Objective-C.
- POSIX extensions are permitted in platform-specific shim files; they must be guarded with appropriate feature-test macros (`_POSIX_C_SOURCE`, etc.).

### Naming conventions

| Entity | Convention | Example |
|---|---|---|
| Public types | `qdb_` prefix, `_t` suffix | `qdb_t`, `qdb_msg_t` |
| Public functions | `qdb_` prefix, snake_case | `qdb_open`, `qdb_push` |
| Public constants / macros | `QDB_` prefix, SCREAMING_SNAKE | `QDB_OK`, `QDB_ERR_IO` |
| Internal functions | `qdb__` double-underscore prefix | `qdb__log_append` |
| Internal types | `qdb__` double-underscore prefix | `qdb__page_t` |
| Local variables | snake_case, descriptive | `msg_len`, `file_offset` |
| Boolean-valued variables | prefixed `is_` or `has_` | `is_dirty`, `has_wal` |

### File layout

- One logical component per file pair (`src/qdb_log.c` + internal header if needed).
- Public API lives exclusively in `include/qdb.h`.
- Internal headers live in `src/` and are never installed.
- Include guards use `QDB_<FILENAME>_H` form.

### Error handling

- All fallible public functions return `int`: `QDB_OK` (0) on success, a negative `QDB_ERR_*` code on failure.
- Functions that return a pointer return `NULL` on failure.
- No exceptions.  No `longjmp`.  No `abort()` except for programmer-error assertions in debug builds.
- Always check return values.  Do not silently discard errors.
- When calling OS APIs, preserve `errno` before doing anything that may overwrite it.

### Memory management

- No dynamic allocation in hot paths where it can be avoided.
- Every allocation must have a matching free on every code path.
- Use `size_t` for sizes and counts.  Never use negative sizes.
- Check for integer overflow before arithmetic that feeds into an allocation size.

### Comments

- Write comments that explain **why**, not what.
- Every public API function must have a doc-comment in `include/qdb.h` describing its behaviour, parameters, return values, and error conditions.
- Internal implementation comments are welcome but not required.
- Do not commit commented-out code.

### Formatting

- 4-space indentation.  No tabs.
- Opening braces on the same line for functions and control flow.
- Maximum line length: 100 characters (soft), 120 characters (hard).
- Always use braces for `if`/`for`/`while` bodies, even single-line ones.

A `.clang-format` file is provided.  Running `clang-format -i` on changed files before committing is strongly encouraged.

---

## Commit Message Conventions

QDB uses the [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) specification.

```
<type>(<scope>): <short summary>

[optional body]

[optional footer(s)]
```

### Types

| Type | When to use |
|---|---|
| `feat` | A new feature |
| `fix` | A bug fix |
| `perf` | A change that improves performance |
| `refactor` | A change that neither fixes a bug nor adds a feature |
| `test` | Adding or correcting tests |
| `docs` | Documentation only changes |
| `build` | Changes to the build system or CI |
| `chore` | Maintenance tasks (dependency bumps, etc.) |

### Rules

- Use the imperative mood in the summary: "add feature" not "added feature".
- Keep the summary line under 72 characters.
- Reference issues in the footer: `Fixes #42` or `Closes #42`.
- Breaking changes must include a `BREAKING CHANGE:` footer and a `!` after the type: `feat!: rename qdb_pop signature`.

### Examples

```
feat(api): add qdb_peek for non-destructive reads

Allows callers to inspect the next message without removing it
from the queue.  Useful for inspection and debugging.

Closes #17
```

```
fix(wal): flush WAL before rotating segment

Previously, a crash between segment rotation and WAL flush could
leave the new segment without its header checksum.

Fixes #31
```

---

## Pull Request Process

1. **One concern per PR.**  A pull request should do one thing.  Mixing unrelated changes makes review harder and bisecting bugs later even harder.

2. **CI must pass.**  All GitHub Actions checks (build, test, warnings-as-errors) must be green before review.

3. **Tests are required.**  Bug fixes must include a regression test.  New features must include tests that would fail without the implementation.

4. **Update documentation.**  If the change affects the public API or on-disk format, update the relevant documentation.

5. **Update CHANGELOG.md.**  Add an entry under `[Unreleased]`.

6. **Keep the diff small.**  Reviewers have limited time.  A 200-line PR gets reviewed carefully; a 2,000-line PR gets skimmed.  Split large changes into a series of smaller PRs where possible.

7. **Respond to review comments.**  Address each comment with either a code change, a clarifying question, or a reasoned rebuttal.  Don't leave comments hanging.

8. **Squash before merge.**  PRs are merged with a single squash commit.  Keep your commit history clean or the maintainer will squash for you.

---

## Testing Requirements

- All public API functions must have test coverage.
- Tests live in `tests/`.  Use the provided lightweight test harness.
- Tests must be deterministic — no relying on timing, no hardcoded ports, no network access.
- Crash-safety tests should use the fork-and-kill pattern to simulate abrupt termination.
- New tests must pass under AddressSanitizer and UBSan.  Build with `-DQDB_SANITIZERS=ON` to enable.
- Aim for meaningful tests, not line-coverage metrics.

---

## Reporting Bugs

Open a GitHub issue with:

- A minimal, self-contained reproducer
- The QDB version (or commit hash)
- The compiler, OS, and architecture
- Expected vs. actual behaviour

For security vulnerabilities, follow the process in [SECURITY.md](SECURITY.md) instead.

---

## Suggesting Features

Open a GitHub issue tagged `enhancement` describing:

- The problem you are trying to solve
- The API or behaviour you are proposing
- Any alternatives you considered

Features that conflict with the project's non-goals (distributed systems, networking, authentication) will not be accepted for v1.
