# QDB Release Checklist

Follow these steps in order for every tagged release.

---

## 1. Confirm scope

- [ ] All planned features for this version are merged to `main`.
- [ ] No known data-loss or corruption bugs are open.
- [ ] `docs/mvp-status.md` accurately reflects what is implemented and what
      is still missing.

---

## 2. Bump version

**Single source of truth:** `VERSION` in the top-level `CMakeLists.txt`.
`include/qdb.h` and `qdb_version.h` (generated at build time) derive from it
automatically — do not edit them directly.

```
# CMakeLists.txt line 3-4:
project(qdb
    VERSION  X.Y.Z   ← change here only
    ...
)
```

After editing `CMakeLists.txt`, reconfigure and verify:

```sh
cmake -B build && grep QDB_VERSION build/include/qdb_version.h
```

---

## 3. Update `CHANGELOG.md`

- [ ] Rename `## [Unreleased]` to `## [X.Y.Z] — YYYY-MM-DD`.
- [ ] Add a new empty `## [Unreleased]` section above it.
- [ ] Update the comparison links at the bottom:

```
[Unreleased]: https://github.com/sjokhio/qdb/compare/vX.Y.Z...HEAD
[X.Y.Z]:      https://github.com/sjokhio/qdb/compare/vPREV...vX.Y.Z
```

---

## 4. Run the full test suite

```sh
# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DQDB_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# ASan + UBSan (Linux / macOS)
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug \
      -DQDB_BUILD_TESTS=ON -DQDB_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

All 12+ suites must pass in both builds before tagging.

---

## 5. Verify CI is green

- [ ] `CI — Linux` passes (all GCC / Clang matrix combinations).
- [ ] `CI — macOS` passes.
- [ ] `CI — Windows` passes (MSVC x64, MSVC Win32, clang-cl).
- [ ] `Fuzz` workflow passes (30-second smoke tests).

Do not tag while any CI job is red.

---

## 6. Create the release commit and tag

```sh
git add CMakeLists.txt CHANGELOG.md
git commit -m "chore: release vX.Y.Z"
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin main --follow-tags
```

---

## 7. Publish the GitHub release

- [ ] Go to **Releases → Draft a new release** on GitHub.
- [ ] Select the tag `vX.Y.Z`.
- [ ] Set the title to `vX.Y.Z`.
- [ ] Paste the `## [X.Y.Z]` section from `CHANGELOG.md` as the release body.
- [ ] Publish.

---

## 8. Post-release

- [ ] Verify the release page looks correct.
- [ ] Close any milestone associated with this version.
- [ ] Open a follow-up issue or milestone for the next planned version.
