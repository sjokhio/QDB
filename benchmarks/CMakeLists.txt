add_executable(bench_throughput throughput.c)

target_link_libraries(bench_throughput PRIVATE qdb::qdb)

target_compile_options(bench_throughput PRIVATE
    ${QDB_WARNING_FLAGS}
)

add_executable(bench_push_pop bench_push_pop.c)

target_link_libraries(bench_push_pop PRIVATE qdb::qdb)

target_compile_options(bench_push_pop PRIVATE
    ${QDB_WARNING_FLAGS}
)
