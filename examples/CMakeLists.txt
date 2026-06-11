add_executable(example_hello hello.c)

target_link_libraries(example_hello PRIVATE qdb::qdb)

target_compile_options(example_hello PRIVATE
    ${QDB_WARNING_FLAGS}
)
