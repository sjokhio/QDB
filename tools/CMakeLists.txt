add_executable(qdbtool qdbtool.c)

target_link_libraries(qdbtool PRIVATE qdb::qdb)

target_compile_options(qdbtool PRIVATE ${QDB_WARNING_FLAGS})

install(TARGETS qdbtool
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
