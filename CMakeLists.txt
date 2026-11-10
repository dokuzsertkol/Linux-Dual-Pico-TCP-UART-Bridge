cmake_minimum_required(VERSION 3.16)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(PicoNetwork LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

add_subdirectory(shared)
add_subdirectory(uart2tcp-client)
add_subdirectory(tcp2uart-client)