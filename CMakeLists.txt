cmake_minimum_required(VERSION 3.18)
project(backplane LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wpedantic)

include(CTest)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
  message(FATAL_ERROR "hiredis was not found. Install libhiredis-dev.")
endif()

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/backplane.proto)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

find_program(PROTOC protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

add_custom_command(
  OUTPUT
    ${GENERATED_DIR}/backplane.pb.cc
    ${GENERATED_DIR}/backplane.pb.h
    ${GENERATED_DIR}/backplane.grpc.pb.cc
    ${GENERATED_DIR}/backplane.grpc.pb.h
  COMMAND ${PROTOC}
  ARGS
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    --cpp_out=${GENERATED_DIR}
    --grpc_out=${GENERATED_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
    ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
)

add_library(backplane_proto
  ${GENERATED_DIR}/backplane.pb.cc
  ${GENERATED_DIR}/backplane.grpc.pb.cc
)

target_include_directories(backplane_proto PUBLIC ${GENERATED_DIR})
target_link_libraries(backplane_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_library(backplane_core
  src/redis_store.cpp
  src/task_print.cpp
)

target_include_directories(backplane_core PUBLIC include ${HIREDIS_INCLUDE_DIR})
target_link_libraries(backplane_core PUBLIC ${HIREDIS_LIBRARY} backplane_proto)

add_executable(backplane_server src/server.cpp)
target_link_libraries(backplane_server PRIVATE backplane_core)

add_executable(backplane_worker src/worker.cpp)
target_link_libraries(backplane_worker PRIVATE backplane_core)

add_executable(backplane_client src/client.cpp)
target_link_libraries(backplane_client PRIVATE backplane_core)

if(BUILD_TESTING)
  add_executable(scheduler_policy_test tests/scheduler_policy_test.cpp)
  target_include_directories(scheduler_policy_test PRIVATE include)
  add_test(NAME scheduler_policy_test COMMAND scheduler_policy_test)
endif()
