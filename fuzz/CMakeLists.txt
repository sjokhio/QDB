# fuzz/CMakeLists.txt — fuzz harness build rules
#
# Three targets:
#   fuzz_record_parser  — log-region fuzzing (valid header, arbitrary records)
#   fuzz_replay         — full-file fuzzing  (header + log both fuzz-controlled)
#   fuzz_header         — header-only fuzzing (4 096 bytes, empty log region)
#
# Plus a seed-generator utility:
#   gen_seeds           — writes initial corpus files from real QDB databases
#
# Build modes
# -----------
# libFuzzer (Clang ≥ 6 or AFL++ afl-clang-fast):
#   cmake -DCMAKE_C_COMPILER=clang -DQDB_BUILD_FUZZ=ON ..
#   cmake --build . --target fuzz_replay fuzz_record_parser fuzz_header
#
# Standalone replay (GCC, MSVC, or explicit -DQDB_FUZZ_STANDALONE=ON):
#   cmake -DQDB_BUILD_FUZZ=ON -DQDB_FUZZ_STANDALONE=ON ..
#   cmake --build . --target fuzz_replay fuzz_record_parser fuzz_header
#   ./fuzz/fuzz_replay corpus/replay/*.qdb   # run on seed files

cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Decide build mode
# ---------------------------------------------------------------------------

if(QDB_FUZZ_STANDALONE)
    set(_fuzz_mode standalone)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(_fuzz_mode libfuzzer)
else()
    # Non-Clang (GCC, MSVC): fall back to standalone replay harness.
    # AFL++ users: use afl-clang-fast (which IS Clang-based) for libfuzzer mode,
    # or set QDB_FUZZ_STANDALONE=ON explicitly for afl-gcc-fast.
    set(_fuzz_mode standalone)
    message(STATUS
        "Fuzz: compiler '${CMAKE_C_COMPILER_ID}' is not Clang — "
        "building standalone harnesses instead of libFuzzer targets.  "
        "Set CC=clang or CC=afl-clang-fast for instrumented builds.")
endif()

if(_fuzz_mode STREQUAL libfuzzer)
    # Compile and link with ASan + UBSan + fuzzer engine.
    # The qdb library itself should also be built with QDB_SANITIZERS=ON for
    # full sanitizer coverage; see docs/fuzzing.md.
    set(_fuzz_compile  -fsanitize=fuzzer,address,undefined
                       -fno-omit-frame-pointer)
    set(_fuzz_link     -fsanitize=fuzzer,address,undefined)
    set(_fuzz_defines  "")
    message(STATUS "Fuzz: libFuzzer mode (ASan + UBSan enabled)")
else()
    set(_fuzz_compile  "")
    set(_fuzz_link     "")
    set(_fuzz_defines  QDB_FUZZ_STANDALONE)
    message(STATUS "Fuzz: standalone replay mode")
endif()

# ---------------------------------------------------------------------------
# Helper that creates one fuzz target
# ---------------------------------------------------------------------------

function(add_fuzz_target _name _source)
    add_executable(${_name} ${_source})

    target_link_libraries(${_name} PRIVATE qdb::qdb)

    # Internal headers needed for qdb__crc32, qdb__put_*, and constants.
    target_include_directories(${_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
    )

    target_compile_definitions(${_name} PRIVATE ${_fuzz_defines})

    if(_fuzz_compile)
        target_compile_options(${_name} PRIVATE ${_fuzz_compile})
    endif()
    if(_fuzz_link)
        target_link_options(${_name} PRIVATE ${_fuzz_link})
    endif()

    # Use a permissive warning set: -Wall only.
    # - libFuzzer injects main() at link time, so -Wmissing-prototypes fires.
    # - Do NOT use -Werror here; the harness files intentionally suppress it.
    if(NOT MSVC)
        target_compile_options(${_name} PRIVATE -Wall)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Fuzz targets
# ---------------------------------------------------------------------------

add_fuzz_target(fuzz_record_parser fuzz_record_parser.c)
add_fuzz_target(fuzz_replay        fuzz_replay.c)
add_fuzz_target(fuzz_header        fuzz_header.c)

# ---------------------------------------------------------------------------
# Seed generator (always standalone — no fuzzing engine needed)
# ---------------------------------------------------------------------------

add_executable(gen_seeds gen_seeds.c)
target_link_libraries(gen_seeds PRIVATE qdb::qdb)
if(NOT MSVC)
    target_compile_options(gen_seeds PRIVATE -Wall)
endif()
