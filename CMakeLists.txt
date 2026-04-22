cmake_minimum_required(VERSION 3.16)

project(rpc_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
include(CTest)

message(STATUS "Using Protobuf ${Protobuf_VERSION}")

set(PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/rpc.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/calc.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/user.proto
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_library(rpc_framework
    ${PROTO_SRCS}
    src/protocol/codec.cpp
    src/common/async_logger.cpp
    src/common/log.cpp
    src/common/thread_pool.cpp
    src/server/service_registry.cpp
    src/server/connection.cpp
    src/server/worker_loop.cpp
    src/server/rpc_server.cpp
    src/client/event_loop.cpp
    src/client/pending_calls.cpp
    src/client/rpc_client.cpp
)

target_include_directories(rpc_framework
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}
        ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(rpc_framework
    PUBLIC
        ${Protobuf_LIBRARIES}
    Threads::Threads
)

add_executable(rpc_server_demo
    src/demo/server_main.cpp
)

target_link_libraries(rpc_server_demo
    PRIVATE
        rpc_framework
)

add_executable(rpc_client_demo
    src/demo/client_main.cpp
)

target_link_libraries(rpc_client_demo
    PRIVATE
        rpc_framework
)

add_executable(rpc_coroutine_client_demo
    src/demo/coroutine_client_main.cpp
)

target_link_libraries(rpc_coroutine_client_demo
    PRIVATE
        rpc_framework
)

if(BUILD_TESTING)
    add_executable(event_loop_test
        tests/event_loop_test.cpp
    )

    target_link_libraries(event_loop_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME event_loop_test COMMAND event_loop_test)

    add_executable(codec_test
        tests/codec_test.cpp
    )

    target_link_libraries(codec_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME codec_test COMMAND codec_test)

    add_executable(service_registry_test
        tests/service_registry_test.cpp
    )

    target_link_libraries(service_registry_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME service_registry_test COMMAND service_registry_test)

    add_executable(e2e_test
        tests/e2e_test.cpp
    )

    target_link_libraries(e2e_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME e2e_test COMMAND e2e_test)

    add_executable(out_of_order_dispatcher_test
        tests/out_of_order_dispatcher_test.cpp
    )

    target_link_libraries(out_of_order_dispatcher_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME out_of_order_dispatcher_test COMMAND out_of_order_dispatcher_test)

    add_executable(call_async_timeout_test
        tests/call_async_timeout_test.cpp
    )

    target_link_libraries(call_async_timeout_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME call_async_timeout_test COMMAND call_async_timeout_test)

    add_executable(call_async_close_test
        tests/call_async_close_test.cpp
    )

    target_link_libraries(call_async_close_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME call_async_close_test COMMAND call_async_close_test)

    add_executable(call_co_basic_test
        tests/call_co_basic_test.cpp
    )

    target_link_libraries(call_co_basic_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME call_co_basic_test COMMAND call_co_basic_test)

    add_executable(call_co_out_of_order_test
        tests/call_co_out_of_order_test.cpp
    )

    target_link_libraries(call_co_out_of_order_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME call_co_out_of_order_test COMMAND call_co_out_of_order_test)

    add_executable(call_co_timeout_test
        tests/call_co_timeout_test.cpp
    )

    target_link_libraries(call_co_timeout_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME call_co_timeout_test COMMAND call_co_timeout_test)

    add_executable(call_co_close_test
        tests/call_co_close_test.cpp
    )

    target_link_libraries(call_co_close_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME call_co_close_test COMMAND call_co_close_test)

    add_executable(call_mixed_waiters_test
        tests/call_mixed_waiters_test.cpp
    )

    target_link_libraries(call_mixed_waiters_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME call_mixed_waiters_test COMMAND call_mixed_waiters_test)

    add_executable(pending_calls_test
        tests/pending_calls_test.cpp
    )

    target_link_libraries(pending_calls_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME pending_calls_test COMMAND pending_calls_test)

    add_executable(call_co_edge_cases_test
        tests/call_co_edge_cases_test.cpp
    )

    target_link_libraries(call_co_edge_cases_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME call_co_edge_cases_test COMMAND call_co_edge_cases_test)

    add_executable(server_connection_test
        tests/server_connection_test.cpp
    )

    target_link_libraries(server_connection_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME server_connection_test COMMAND server_connection_test)

    add_executable(server_connection_coroutine_test
        tests/server_connection_coroutine_test.cpp
    )

    target_link_libraries(server_connection_coroutine_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME server_connection_coroutine_test COMMAND server_connection_coroutine_test)

    add_executable(server_worker_loop_test
        tests/server_worker_loop_test.cpp
    )

    target_link_libraries(server_worker_loop_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME server_worker_loop_test COMMAND server_worker_loop_test)

    add_executable(server_multi_connection_test
        tests/server_multi_connection_test.cpp
    )

    target_link_libraries(server_multi_connection_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME server_multi_connection_test COMMAND server_multi_connection_test)

    add_executable(server_multi_worker_loop_test
        tests/server_multi_worker_loop_test.cpp
    )

    target_link_libraries(server_multi_worker_loop_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME server_multi_worker_loop_test COMMAND server_multi_worker_loop_test)

    add_executable(server_thread_pool_integration_test
        tests/server_thread_pool_integration_test.cpp
    )

    target_link_libraries(server_thread_pool_integration_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME server_thread_pool_integration_test COMMAND server_thread_pool_integration_test)

    add_executable(heartbeat_test
        tests/heartbeat_test.cpp
    )

    target_link_libraries(heartbeat_test
        PRIVATE
            rpc_framework
            Threads::Threads
    )

    add_test(NAME heartbeat_test COMMAND heartbeat_test)
endif()

add_executable(rpc_benchmark
    benchmarks/benchmark_stats.cpp
    benchmarks/rpc_benchmark.cpp
)

target_link_libraries(rpc_benchmark
    PRIVATE
        rpc_framework
        Threads::Threads
)

add_executable(rpc_thread_pool_benchmark
    benchmarks/benchmark_stats.cpp
    benchmarks/rpc_thread_pool_benchmark.cpp
)

target_link_libraries(rpc_thread_pool_benchmark
    PRIVATE
        rpc_framework
        Threads::Threads
)

add_executable(rpc_benchmark_pipeline
    benchmarks/rpc_benchmark_pipeline.cpp
)

target_link_libraries(rpc_benchmark_pipeline
    PRIVATE
        rpc_framework
        Threads::Threads
)

add_executable(rpc_benchmark_conn_pool
    benchmarks/benchmark_stats.cpp
    benchmarks/rpc_benchmark_conn_pool.cpp
)

target_link_libraries(rpc_benchmark_conn_pool
    PRIVATE
        rpc_framework
        Threads::Threads
)

add_executable(async_logger_benchmark
    benchmarks/async_logger_benchmark.cpp
)

target_link_libraries(async_logger_benchmark
    PRIVATE
        rpc_framework
        Threads::Threads
)
