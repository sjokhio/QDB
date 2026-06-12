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
