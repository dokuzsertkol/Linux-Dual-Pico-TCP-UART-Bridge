cmake_minimum_required(VERSION 3.16)
project(NetworkServer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../shared
                 ${CMAKE_CURRENT_BINARY_DIR}/shared)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../server
                 ${CMAKE_CURRENT_BINARY_DIR}/server)
