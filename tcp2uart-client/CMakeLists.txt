add_executable(tcp2uart-client
    tcp2uart-client.c
)

target_include_directories(tcp2uart-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(tcp2uart-client PRIVATE
    pico_stdlib
    pico_multicore
    hardware_uart
    hardware_dma
    pico_cyw43_arch_lwip_threadsafe_background
    protocol
)

pico_add_extra_outputs(tcp2uart-client)
pico_enable_stdio_usb(tcp2uart-client 1)
pico_enable_stdio_uart(tcp2uart-client 0)