add_library(protocol STATIC
    protocol.c
)

target_include_directories(protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)