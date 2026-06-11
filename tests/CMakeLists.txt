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
