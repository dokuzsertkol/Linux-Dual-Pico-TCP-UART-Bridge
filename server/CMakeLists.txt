add_executable(server
    server.c
)

target_include_directories(server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(server PRIVATE
    protocol
)