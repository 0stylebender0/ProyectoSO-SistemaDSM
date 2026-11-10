cmake_minimum_required(VERSION 3.16)
project(dsm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(dsm_node
    src/main.cpp
    src/config.cpp
    src/memory_manager.cpp
    src/network.cpp
    src/protocol.cpp
)

target_include_directories(dsm_node PRIVATE include)
target_compile_options(dsm_node PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
target_link_libraries(dsm_node PRIVATE Threads::Threads)

enable_testing()

add_executable(test_protocol
    tests/test_protocol.cpp
    src/protocol.cpp
)

target_include_directories(test_protocol PRIVATE include)
target_compile_options(test_protocol PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME protocol COMMAND test_protocol)

add_executable(dsm_status apps/status_client.cpp src/protocol.cpp)
target_include_directories(dsm_status PRIVATE include)
target_compile_options(dsm_status PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_network tests/test_network.cpp src/network.cpp src/protocol.cpp)
target_include_directories(test_network PRIVATE include)
target_compile_options(test_network PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_network PRIVATE Threads::Threads)

add_test(NAME network COMMAND test_network)
set_tests_properties(network PROPERTIES TIMEOUT 20)
