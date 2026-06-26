cmake_minimum_required(VERSION 3.20)

project(qdb
    VERSION      1.2.0
    DESCRIPTION  "Lightweight embedded persistent message queue library"
    HOMEPAGE_URL "https://github.com/sjokhio/qdb"
    LANGUAGES    C
)

# ---------------------------------------------------------------------------
# Standards and policies
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD          17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# Export compile_commands.json for editor tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(QDB_BUILD_TESTS        "Build the test suite"              ON)
option(QDB_BUILD_TOOLS        "Build qdbtool command-line utility" ON)
option(QDB_BUILD_EXAMPLES     "Build example programs"            ON)
option(QDB_BUILD_BENCHMARKS   "Build benchmark programs"          OFF)
option(QDB_BUILD_FUZZ         "Build fuzz harnesses"              OFF)
option(QDB_FUZZ_STANDALONE    "Build fuzz harnesses as standalone file-replay tools (no fuzzing engine)" OFF)
option(QDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(QDB_SANITIZERS         "Enable AddressSanitizer + UBSan"   OFF)

# ---------------------------------------------------------------------------
# Compiler warning flags
# ---------------------------------------------------------------------------

if(MSVC)
    set(QDB_WARNING_FLAGS /W4 /WX)
else()
    set(QDB_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wdouble-promotion
        -Wformat=2
        -Wundef
        -Wconversion
        -Wnull-dereference
        -Wcast-align
    )
    if(QDB_WARNINGS_AS_ERRORS)
        list(APPEND QDB_WARNING_FLAGS -Werror)
    endif()
    if(MSVC)
        # already handled above
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        list(APPEND QDB_WARNING_FLAGS
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wmissing-declarations
        )
    endif()
endif()

if(MSVC AND QDB_WARNINGS_AS_ERRORS)
    list(APPEND QDB_WARNING_FLAGS /WX)
endif()

# ---------------------------------------------------------------------------
# Sanitizer flags (non-MSVC only)
# ---------------------------------------------------------------------------

if(QDB_SANITIZERS AND NOT MSVC)
    set(QDB_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
else()
    set(QDB_SANITIZER_FLAGS "")
endif()

# ---------------------------------------------------------------------------
# Library target: qdb
# ---------------------------------------------------------------------------

add_library(qdb STATIC
    src/qdb.c
    src/qdb_crc32.c
    src/qdb_io.c
    src/qdb_platform.c
    src/qdb_state.c
    src/qdb_replay.c
)

add_library(qdb::qdb ALIAS qdb)

target_include_directories(qdb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Warning flags: PRIVATE — do not force -Wall/-Werror onto consumers.
target_compile_options(qdb PRIVATE ${QDB_WARNING_FLAGS})

# Sanitizer flags: PUBLIC compile + INTERFACE link.
#
# A static library does not produce a final link step, so PRIVATE link
# options are silently ignored by the linker.  Making compile options PUBLIC
# ensures every consumer TU is instrumented consistently.  Making link
# options INTERFACE ensures every consumer executable pulls in the sanitizer
# runtime (-fsanitize=address,undefined resolves __asan_* / __ubsan_*
# symbols that the instrumented archive objects reference).
#
# When QDB_SANITIZERS=OFF, QDB_SANITIZER_FLAGS is empty and these calls
# are no-ops, so normal builds are unaffected.
if(QDB_SANITIZER_FLAGS)
    target_compile_options(qdb PUBLIC    ${QDB_SANITIZER_FLAGS})
    target_link_options(qdb    INTERFACE ${QDB_SANITIZER_FLAGS})
endif()

# Windows: link against the system socket/timing libraries used internally
if(WIN32)
    target_link_libraries(qdb PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Version header (generated)
# ---------------------------------------------------------------------------

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qdb_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/qdb_version.h"
    @ONLY
)

target_include_directories(qdb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(QDB_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(QDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(QDB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(QDB_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Fuzz harnesses
# ---------------------------------------------------------------------------

if(QDB_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS qdb
    EXPORT  qdbTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/qdb.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# qdb.h includes qdb_version.h; install the generated copy alongside it.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/qdb_version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT qdbTargets
    FILE        qdbTargets.cmake
    NAMESPACE   qdb::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qdb
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qdbConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qdbConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qdb
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/qdbConfigVersion.cmake"
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/qdbConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/qdbConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qdb
)

# pkg-config file — POSIX only; Windows consumers use the CMake package.
if(NOT WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qdb.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/qdb.pc"
        @ONLY
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qdb.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    )
endif()
