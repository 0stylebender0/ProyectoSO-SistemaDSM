cmake_minimum_required(VERSION 3.16)
project(dsm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(dsm_node
    src/main.cpp
    src/config.cpp
    src/memory_manager.cpp
    src/fault_queue.cpp
    src/signal_handler.cpp
    src/network.cpp
    src/node.cpp
    src/pager.cpp
    src/dsm_node.cpp
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
    src/node.cpp
)

target_include_directories(test_protocol PRIVATE include)
target_compile_options(test_protocol PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_protocol PRIVATE Threads::Threads)

add_test(NAME protocol COMMAND test_protocol)

add_executable(dsm_status apps/status_client.cpp src/protocol.cpp src/node.cpp)
target_include_directories(dsm_status PRIVATE include)
target_compile_options(dsm_status PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsm_status PRIVATE Threads::Threads)

add_executable(test_network tests/test_network.cpp src/network.cpp src/protocol.cpp src/node.cpp)
target_include_directories(test_network PRIVATE include)
target_compile_options(test_network PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_network PRIVATE Threads::Threads)

add_test(NAME network COMMAND test_network)
set_tests_properties(network PROPERTIES TIMEOUT 20)

add_executable(test_membership tests/test_membership.cpp src/network.cpp src/protocol.cpp src/node.cpp)
target_include_directories(test_membership PRIVATE include)
target_compile_options(test_membership PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_membership PRIVATE Threads::Threads)

add_test(NAME membership COMMAND test_membership)
set_tests_properties(membership PROPERTIES TIMEOUT 30)

add_executable(test_fault tests/test_fault.cpp src/memory_manager.cpp src/fault_queue.cpp src/signal_handler.cpp)
target_include_directories(test_fault PRIVATE include)
target_compile_options(test_fault PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_fault PRIVATE Threads::Threads)

add_test(NAME fault COMMAND test_fault)
set_tests_properties(fault PROPERTIES TIMEOUT 20)

add_executable(test_pager
    tests/test_pager.cpp
    src/pager.cpp
    src/memory_manager.cpp
    src/fault_queue.cpp
    src/signal_handler.cpp
)
target_include_directories(test_pager PRIVATE include)
target_compile_options(test_pager PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_pager PRIVATE Threads::Threads)

add_test(NAME pager COMMAND test_pager)
set_tests_properties(pager PROPERTIES TIMEOUT 20)

add_executable(test_page_transfer
    tests/test_page_transfer.cpp
    src/network.cpp
    src/protocol.cpp
    src/node.cpp
    src/pager.cpp
    src/memory_manager.cpp
    src/fault_queue.cpp
    src/signal_handler.cpp
    src/dsm_node.cpp
)
target_include_directories(test_page_transfer PRIVATE include)
target_compile_options(test_page_transfer PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_page_transfer PRIVATE Threads::Threads)

add_test(NAME page_transfer COMMAND test_page_transfer)
set_tests_properties(page_transfer PROPERTIES TIMEOUT 20)

add_executable(test_msi
    tests/test_msi.cpp
    src/network.cpp src/protocol.cpp src/node.cpp src/pager.cpp
    src/memory_manager.cpp src/fault_queue.cpp src/signal_handler.cpp src/dsm_node.cpp
)
target_include_directories(test_msi PRIVATE include)
target_compile_options(test_msi PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_msi PRIVATE Threads::Threads)
add_test(NAME msi COMMAND test_msi)
set_tests_properties(msi PROPERTIES TIMEOUT 40)

add_executable(test_concurrency
    tests/test_concurrency.cpp
    src/network.cpp src/protocol.cpp src/node.cpp src/pager.cpp
    src/memory_manager.cpp src/fault_queue.cpp src/signal_handler.cpp src/dsm_node.cpp
)
target_include_directories(test_concurrency PRIVATE include)
target_compile_options(test_concurrency PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_concurrency PRIVATE Threads::Threads)
add_test(NAME concurrency COMMAND test_concurrency)
set_tests_properties(concurrency PROPERTIES TIMEOUT 40)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME console COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/test_console.py"
        "$<TARGET_FILE:dsm_node>" "$<TARGET_FILE:dsm_status>"
        "${CMAKE_SOURCE_DIR}/config/dsm.conf"
        "${CMAKE_SOURCE_DIR}/scripts/demo_local.sh"
    )
    set_tests_properties(console PROPERTIES TIMEOUT 40)
endif()
