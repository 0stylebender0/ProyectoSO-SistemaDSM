cmake_minimum_required(VERSION 3.16)
project(dsm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Uso: cmake -B build-asan -DDSM_SANITIZERS=address,undefined
#      cmake -B build-tsan -DDSM_SANITIZERS=thread
# Los flujos DSM usan SIGSEGV y pila alternativa; ASan/UBSan se ejecutan
# sobre toda la suite y TSan de forma selectiva (ver scripts/run_sanitizers.sh).
set(DSM_SANITIZERS "" CACHE STRING
    "Sanitizers de GCC/Clang, por ejemplo address,undefined o thread")
if(DSM_SANITIZERS)
    add_compile_options(-fsanitize=${DSM_SANITIZERS} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${DSM_SANITIZERS})
    if(DSM_SANITIZERS MATCHES "address|undefined")
        add_compile_options(-fno-sanitize-recover=all)
    endif()
endif()

find_package(Threads REQUIRED)

add_library(dsm STATIC
    src/dsm.cpp
    src/config.cpp
    src/memory_manager.cpp
    src/fault_queue.cpp
    src/signal_handler.cpp
    src/network.cpp
    src/heartbeat.cpp
    src/node.cpp
    src/pager.cpp
    src/dsm_node.cpp
    src/protocol.cpp
    src/status_report.cpp
)

target_include_directories(dsm PUBLIC include)
target_compile_options(dsm PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsm PUBLIC Threads::Threads)

add_executable(dsm_node src/main.cpp src/demo_app.cpp)
target_compile_options(dsm_node PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsm_node PRIVATE dsm)

add_executable(dsm_app apps/dsm_app.cpp)
target_compile_options(dsm_app PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsm_app PRIVATE dsm)

add_executable(dsm_validate apps/dsm_validate.cpp)
target_compile_options(dsm_validate PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsm_validate PRIVATE dsm)

add_executable(dsm_array apps/dsm_array.cpp)
target_compile_options(dsm_array PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsm_array PRIVATE dsm)

add_executable(dsm_matrix apps/dsm_matrix.cpp)
target_compile_options(dsm_matrix PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsm_matrix PRIVATE dsm)

enable_testing()

add_executable(test_api tests/test_api.cpp)
target_compile_options(test_api PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_api PRIVATE dsm)
add_test(NAME api COMMAND test_api "$<TARGET_FILE:dsm_app>")
set_tests_properties(api PROPERTIES TIMEOUT 60)

add_executable(test_global tests/test_global.cpp)
target_compile_options(test_global PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_global PRIVATE dsm)
add_test(NAME global COMMAND test_global)
set_tests_properties(global PROPERTIES TIMEOUT 25)

add_executable(test_release tests/test_release.cpp)
target_compile_options(test_release PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_release PRIVATE dsm)
add_test(NAME release COMMAND test_release)
set_tests_properties(release PROPERTIES TIMEOUT 30)

add_executable(test_heartbeat tests/test_heartbeat.cpp)
target_compile_options(test_heartbeat PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_heartbeat PRIVATE dsm)
add_test(NAME heartbeat COMMAND test_heartbeat)
set_tests_properties(heartbeat PROPERTIES TIMEOUT 30)

add_executable(test_protocol
    tests/test_protocol.cpp
    src/protocol.cpp
    src/node.cpp
)

target_include_directories(test_protocol PRIVATE include)
target_compile_options(test_protocol PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_protocol PRIVATE Threads::Threads)

add_test(NAME protocol COMMAND test_protocol)

add_executable(dsm_status apps/status_client.cpp)
target_compile_options(dsm_status PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsm_status PRIVATE dsm)

add_executable(test_network tests/test_network.cpp src/network.cpp src/heartbeat.cpp src/protocol.cpp src/node.cpp)
target_include_directories(test_network PRIVATE include)
target_compile_options(test_network PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_network PRIVATE Threads::Threads)

add_test(NAME network COMMAND test_network)
set_tests_properties(network PROPERTIES TIMEOUT 20)

add_executable(test_membership tests/test_membership.cpp src/network.cpp src/heartbeat.cpp src/protocol.cpp src/node.cpp)
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
    src/heartbeat.cpp
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
    src/network.cpp src/heartbeat.cpp src/protocol.cpp src/node.cpp src/pager.cpp
    src/memory_manager.cpp src/fault_queue.cpp src/signal_handler.cpp src/dsm_node.cpp
)
target_include_directories(test_msi PRIVATE include)
target_compile_options(test_msi PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_msi PRIVATE Threads::Threads)
add_test(NAME msi COMMAND test_msi)
set_tests_properties(msi PROPERTIES TIMEOUT 40)

add_executable(test_concurrency
    tests/test_concurrency.cpp
    src/network.cpp src/heartbeat.cpp src/protocol.cpp src/node.cpp src/pager.cpp
    src/memory_manager.cpp src/fault_queue.cpp src/signal_handler.cpp src/dsm_node.cpp
)
target_include_directories(test_concurrency PRIVATE include)
target_compile_options(test_concurrency PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(test_concurrency PRIVATE Threads::Threads)
add_test(NAME concurrency COMMAND test_concurrency)
set_tests_properties(concurrency PROPERTIES TIMEOUT 60)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME console COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/test_console.py"
        "$<TARGET_FILE:dsm_node>" "$<TARGET_FILE:dsm_status>"
        "${CMAKE_SOURCE_DIR}/config/dsm.conf"
        "${CMAKE_SOURCE_DIR}/scripts/demo_local.sh"
    )
    set_tests_properties(console PROPERTIES TIMEOUT 40)

    add_test(NAME validation COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/test_validate.py"
        "$<TARGET_FILE:dsm_validate>"
    )
    set_tests_properties(validation PROPERTIES TIMEOUT 60)

    add_test(NAME apps COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/test_apps.py"
        "$<TARGET_FILE:dsm_array>" "$<TARGET_FILE:dsm_matrix>"
    )
    set_tests_properties(apps PROPERTIES TIMEOUT 60)
endif()

add_test(NAME final_demo COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/demo_final.sh")
set_tests_properties(final_demo PROPERTIES TIMEOUT 60)
