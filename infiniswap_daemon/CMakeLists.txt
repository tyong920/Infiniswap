cmake_minimum_required(VERSION 3.22)

project(InfiniswapProvider VERSION 0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(IBVERBS REQUIRED IMPORTED_TARGET libibverbs)
pkg_check_modules(RDMACM REQUIRED IMPORTED_TARGET librdmacm)
pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)

add_library(
  infiniswap-protocol STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/infiniswap_protocol.c)
target_compile_features(infiniswap-protocol PUBLIC c_std_11)
target_compile_options(infiniswap-protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
  infiniswap-protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_library(
  infiniswap-placement STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/infiniswap_placement.c)
target_compile_features(infiniswap-placement PUBLIC c_std_11)
target_compile_options(infiniswap-placement PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
  infiniswap-placement PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_library(infiniswap-memory STATIC infiniswap_memory_manager.c)
target_compile_features(infiniswap-memory PUBLIC c_std_11)
target_compile_definitions(infiniswap-memory PUBLIC _POSIX_C_SOURCE=200809L)
target_compile_options(infiniswap-memory PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
  infiniswap-memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(infiniswap-memory PUBLIC Threads::Threads)

add_library(
  infiniswap-observability STATIC
  infiniswap_observability.c
  infiniswap_observability_server.c)
target_compile_features(infiniswap-observability PUBLIC c_std_11)
target_compile_definitions(
  infiniswap-observability PUBLIC _POSIX_C_SOURCE=200809L)
target_compile_options(
  infiniswap-observability PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
  infiniswap-observability PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  infiniswap-observability
  PUBLIC infiniswap-memory infiniswap-protocol Threads::Threads)

add_library(
  infiniswap-control STATIC
  infiniswap_auth.c
  infiniswap_provider_session.c)
target_compile_features(infiniswap-control PUBLIC c_std_11)
target_compile_definitions(infiniswap-control PUBLIC _POSIX_C_SOURCE=200809L)
target_compile_options(infiniswap-control PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
  infiniswap-control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  infiniswap-control PUBLIC infiniswap-protocol PkgConfig::CRYPTO)

add_library(infiniswap-consumer-liveness STATIC infiniswap_consumer_liveness.c)
target_compile_features(infiniswap-consumer-liveness PUBLIC c_std_11)
target_compile_options(
  infiniswap-consumer-liveness PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
  infiniswap-consumer-liveness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(infiniswap-provider-core STATIC rdma-common.c)
target_compile_features(infiniswap-provider-core PUBLIC c_std_11)
target_compile_definitions(
  infiniswap-provider-core
  PUBLIC
    _GNU_SOURCE
    _POSIX_C_SOURCE=200809L)
target_compile_options(
  infiniswap-provider-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
  infiniswap-provider-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  infiniswap-provider-core
  PUBLIC
    PkgConfig::IBVERBS
    PkgConfig::RDMACM
    PkgConfig::CRYPTO
    Threads::Threads
    infiniswap-consumer-liveness
    infiniswap-control
    infiniswap-memory
    infiniswap-observability)

add_executable(infiniswap-daemon infiniswap-daemon.c)
target_compile_options(infiniswap-daemon PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(infiniswap-daemon PRIVATE infiniswap-provider-core)

install(TARGETS infiniswap-daemon RUNTIME DESTINATION sbin)
install(
  FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/consumer.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/consumer.example.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/consumer-v1.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/consumer-v1.example.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/consumer-v2.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/consumer-v2.example.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/consumer-remote-only.example.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider.example.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-v1.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-v1.example.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-directory.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-directory.example.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-directory-v1.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-directory-v1.example.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/status.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/status-v1.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/status-v2.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/status-v3.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/status-v4.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/status-v5.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-status.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-memory.example.conf
  DESTINATION share/infiniswap)

if(BUILD_TESTING)
  add_executable(auth-session-test tests/auth_session_test.c)
  target_compile_options(auth-session-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(auth-session-test PRIVATE infiniswap-control)
  add_test(NAME auth-session COMMAND auth-session-test)

  add_executable(protocol-test tests/protocol_test.c)
  target_compile_options(protocol-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(protocol-test PRIVATE infiniswap-protocol)
  add_test(NAME protocol COMMAND protocol-test)

  add_executable(placement-test tests/placement_test.c)
  target_compile_options(placement-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(placement-test PRIVATE infiniswap-placement)
  add_test(NAME placement COMMAND placement-test)

  add_executable(
    protocol-previous-build-test
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/infiniswap_protocol.c
    tests/protocol_previous_build_test.c)
  target_compile_definitions(
    protocol-previous-build-test
    PRIVATE
      IS_PROTOCOL_MINOR_CURRENT=0)
  target_compile_options(
    protocol-previous-build-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(
    protocol-previous-build-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
  add_test(NAME protocol-previous-build COMMAND protocol-previous-build-test)

  add_executable(memory-manager-test tests/memory_manager_test.c)
  target_compile_options(memory-manager-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(memory-manager-test PRIVATE infiniswap-memory)
  add_test(NAME memory-manager COMMAND memory-manager-test)

  add_executable(consumer-liveness-test tests/consumer_liveness_test.c)
  target_compile_options(
    consumer-liveness-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(
    consumer-liveness-test PRIVATE infiniswap-consumer-liveness)
  add_test(NAME consumer-liveness COMMAND consumer-liveness-test)

  add_executable(provider-observability-test tests/provider_observability_test.c)
  target_compile_options(
    provider-observability-test
    PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  target_link_libraries(
    provider-observability-test PRIVATE infiniswap-observability)
  add_test(
    NAME provider-observability
    COMMAND
      provider-observability-test
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/provider-status-v1.json
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/provider-metrics-v1.txt)

  add_executable(rdma-common-test tests/rdma_common_test.c)
  target_compile_options(rdma-common-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(rdma-common-test PRIVATE infiniswap-provider-core)
  target_link_options(
    rdma-common-test PRIVATE "-Wl,--wrap=is_memory_manager_disconnect")
  add_test(NAME rdma-common COMMAND rdma-common-test)
endif()
