add_executable(test_basic test_basic.c)

target_link_libraries(test_basic PRIVATE qdb::qdb)

target_compile_options(test_basic PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_basic PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME basic COMMAND test_basic)

# ---------------------------------------------------------------------------
# Storage-layer tests
# ---------------------------------------------------------------------------

add_executable(test_storage test_storage.c)

target_include_directories(test_storage PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_storage PRIVATE qdb::qdb)

target_compile_options(test_storage PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_storage PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME storage COMMAND test_storage
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# Replay / state reconstruction tests
# ---------------------------------------------------------------------------

add_executable(test_replay test_replay.c)

target_include_directories(test_replay PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_replay PRIVATE qdb::qdb)

target_compile_options(test_replay PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_replay PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME replay COMMAND test_replay
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_push() tests
# ---------------------------------------------------------------------------

add_executable(test_push test_push.c)

target_include_directories(test_push PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_push PRIVATE qdb::qdb)

target_compile_options(test_push PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_push PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME push COMMAND test_push
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_pop() tests
# ---------------------------------------------------------------------------

add_executable(test_pop test_pop.c)

target_include_directories(test_pop PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_pop PRIVATE qdb::qdb)

target_compile_options(test_pop PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_pop PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME pop COMMAND test_pop
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_pop_any() tests
# ---------------------------------------------------------------------------

add_executable(test_pop_any test_pop_any.c)

target_include_directories(test_pop_any PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_pop_any PRIVATE qdb::qdb)

target_compile_options(test_pop_any PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_pop_any PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME pop_any COMMAND test_pop_any
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_ack() tests
# ---------------------------------------------------------------------------

add_executable(test_ack test_ack.c)

target_include_directories(test_ack PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_ack PRIVATE qdb::qdb)

target_compile_options(test_ack PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_ack PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME ack COMMAND test_ack
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_nack() tests
# ---------------------------------------------------------------------------

add_executable(test_nack test_nack.c)

target_include_directories(test_nack PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_nack PRIVATE qdb::qdb)

target_compile_options(test_nack PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_nack PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME nack COMMAND test_nack
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_process_expired_leases() tests
# ---------------------------------------------------------------------------

add_executable(test_expire test_expire.c)

target_include_directories(test_expire PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_expire PRIVATE qdb::qdb)

target_compile_options(test_expire PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_expire PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME expire COMMAND test_expire
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_stats() / qdb_queue_stats() tests
# ---------------------------------------------------------------------------

add_executable(test_stats test_stats.c)

target_include_directories(test_stats PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_stats PRIVATE qdb::qdb)

target_compile_options(test_stats PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_stats PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME stats COMMAND test_stats
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_open_ex() / configurable lease timeout tests
# ---------------------------------------------------------------------------

add_executable(test_open_ex test_open_ex.c)

target_include_directories(test_open_ex PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_open_ex PRIVATE qdb::qdb)

target_compile_options(test_open_ex PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_open_ex PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME open_ex COMMAND test_open_ex
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_open_err() tests
# ---------------------------------------------------------------------------

add_executable(test_open_err test_open_err.c)

target_include_directories(test_open_err PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_open_err PRIVATE qdb::qdb)

target_compile_options(test_open_err PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_open_err PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME open_err COMMAND test_open_err
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# Multi-process test foundation
# ---------------------------------------------------------------------------

add_executable(test_mp test_mp.c)

target_link_libraries(test_mp PRIVATE qdb::qdb)

target_compile_options(test_mp PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_mp PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME mp COMMAND test_mp
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(mp PROPERTIES TIMEOUT 15)

# ---------------------------------------------------------------------------
# qdb_compact() tests
# ---------------------------------------------------------------------------

add_executable(test_compact test_compact.c)

target_link_libraries(test_compact PRIVATE qdb::qdb)

target_compile_options(test_compact PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_compact PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME compact COMMAND test_compact
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# API boundary tests (empty payload, max/over-limit queue name, compact edge cases)
# ---------------------------------------------------------------------------

add_executable(test_boundaries test_boundaries.c)

target_link_libraries(test_boundaries PRIVATE qdb::qdb)

target_compile_options(test_boundaries PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_boundaries PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME boundaries COMMAND test_boundaries
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdb_queue_list() tests
# ---------------------------------------------------------------------------

add_executable(test_queue_list test_queue_list.c)

target_link_libraries(test_queue_list PRIVATE qdb::qdb)

target_compile_options(test_queue_list PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_queue_list PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME queue_list COMMAND test_queue_list
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# qdbtool integration tests
# ---------------------------------------------------------------------------

if(QDB_BUILD_TOOLS)

add_executable(test_qdbtool test_qdbtool.c)

target_link_libraries(test_qdbtool PRIVATE qdb::qdb)

target_compile_options(test_qdbtool PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_qdbtool PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

target_compile_definitions(test_qdbtool PRIVATE
    QDBTOOL_EXE="$<TARGET_FILE:qdbtool>"
)

add_dependencies(test_qdbtool qdbtool)

add_test(NAME qdbtool COMMAND test_qdbtool
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(qdbtool PROPERTIES TIMEOUT 30)

endif()

# ---------------------------------------------------------------------------
# qdb_compact_recommended() tests
# ---------------------------------------------------------------------------

add_executable(test_compact_recommended test_compact_recommended.c)

target_link_libraries(test_compact_recommended PRIVATE qdb::qdb)

target_compile_options(test_compact_recommended PRIVATE
    ${QDB_WARNING_FLAGS}
    ${QDB_SANITIZER_FLAGS}
)

target_link_options(test_compact_recommended PRIVATE
    ${QDB_SANITIZER_FLAGS}
)

add_test(NAME compact_recommended COMMAND test_compact_recommended
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ---------------------------------------------------------------------------
# pkg-config install smoke test (non-Windows, requires pkg-config/pkgconf)
# ---------------------------------------------------------------------------

if(NOT WIN32)
    find_program(QDB_PKG_CONFIG_EXECUTABLE NAMES pkg-config pkgconf)
    if(QDB_PKG_CONFIG_EXECUTABLE)
        add_test(
            NAME pkgconfig_install
            COMMAND ${CMAKE_COMMAND}
                    -DBUILD_DIR=${CMAKE_BINARY_DIR}
                    -DPROJECT_VERSION=${PROJECT_VERSION}
                    -DPKG_CONFIG=${QDB_PKG_CONFIG_EXECUTABLE}
                    -DC_COMPILER=${CMAKE_C_COMPILER}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test_pkgconfig.cmake
        )
        set_tests_properties(pkgconfig_install PROPERTIES TIMEOUT 60)
    else()
        message(STATUS "pkg-config not found — skipping pkgconfig_install test")
    endif()
endif()
