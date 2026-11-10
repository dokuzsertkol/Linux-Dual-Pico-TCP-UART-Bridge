add_executable(uart2tcp-client
    uart2tcp-client.c
)

target_include_directories(uart2tcp-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(uart2tcp-client PRIVATE
    pico_stdlib
    hardware_uart
    hardware_dma
    pico_cyw43_arch_lwip_threadsafe_background
    protocol
)

pico_add_extra_outputs(uart2tcp-client)
pico_enable_stdio_usb(uart2tcp-client 1)
pico_enable_stdio_uart(uart2tcp-client 0)