cmake_minimum_required(VERSION 3.16)

project(rpc_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)

message(STATUS "Using Protobuf ${Protobuf_VERSION}")

set(PROTO_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/rpc.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/calc.proto
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_library(rpc_framework
    ${PROTO_SRCS}
    src/protocol/codec.cpp
    src/server/service_registry.cpp
    src/server/rpc_server.cpp
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
