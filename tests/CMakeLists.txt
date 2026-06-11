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
