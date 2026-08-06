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

set(INFINISWAP_MAX_REMOTE_MEMORY_GB 32 CACHE STRING
    "Maximum Remote Memory in GiB contributed by this Memory Provider")
set(INFINISWAP_REMOTE_MEMORY_EVICT_GB 8 CACHE STRING
    "Remote Memory eviction threshold in GiB")
set(INFINISWAP_EVICT_HIT_LIMIT 1 CACHE STRING
    "Consecutive low-memory samples before eviction")
set(INFINISWAP_REMOTE_MEMORY_EXPAND_GB 16 CACHE STRING
    "Remote Memory expansion threshold in GiB")
set(INFINISWAP_EXPAND_HIT_LIMIT 20 CACHE STRING
    "Consecutive high-memory samples before expansion")
set(INFINISWAP_MEASURED_FREE_MEM_WEIGHT 0.7 CACHE STRING
    "Weight of the current free-memory sample")

if(INFINISWAP_MAX_REMOTE_MEMORY_GB LESS 1 OR
   INFINISWAP_MAX_REMOTE_MEMORY_GB GREATER 128)
  message(FATAL_ERROR
          "INFINISWAP_MAX_REMOTE_MEMORY_GB must be between 1 and 128")
endif()

add_library(
  infiniswap-protocol STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/infiniswap_protocol.c)
target_compile_features(infiniswap-protocol PUBLIC c_std_11)
target_compile_definitions(
  infiniswap-protocol
  PRIVATE USER_MAX_REMOTE_MEMORY=${INFINISWAP_MAX_REMOTE_MEMORY_GB})
target_compile_options(infiniswap-protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_include_directories(
  infiniswap-protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../common)

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

add_library(infiniswap-provider-core STATIC rdma-common.c)
target_compile_features(infiniswap-provider-core PUBLIC c_std_11)
target_compile_definitions(
  infiniswap-provider-core
  PUBLIC
    _GNU_SOURCE
    _POSIX_C_SOURCE=200809L
    USER_MAX_CLIENT=1
    USER_MAX_REMOTE_MEMORY=${INFINISWAP_MAX_REMOTE_MEMORY_GB}
    USER_REMOTE_MEMORY_EVICT=${INFINISWAP_REMOTE_MEMORY_EVICT_GB}
    USER_EVICT_HIT_LIMIT=${INFINISWAP_EVICT_HIT_LIMIT}
    USER_REMOTE_MEMORY_EXPAND=${INFINISWAP_REMOTE_MEMORY_EXPAND_GB}
    USER_EXPAND_HIT_LIMIT=${INFINISWAP_EXPAND_HIT_LIMIT}
    USER_MEASURED_FREE_MEM_WEIGHT=${INFINISWAP_MEASURED_FREE_MEM_WEIGHT})
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
    infiniswap-control)

add_executable(infiniswap-daemon infiniswap-daemon.c)
target_compile_options(infiniswap-daemon PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(infiniswap-daemon PRIVATE infiniswap-provider-core)

install(TARGETS infiniswap-daemon RUNTIME DESTINATION sbin)
install(
  FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-directory.schema.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../config/provider-directory.example.json
  DESTINATION share/infiniswap)

if(BUILD_TESTING)
  add_executable(auth-session-test tests/auth_session_test.c)
  target_compile_options(auth-session-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(auth-session-test PRIVATE infiniswap-control)
  add_test(NAME auth-session COMMAND auth-session-test)

  add_executable(protocol-test tests/protocol_test.c)
  target_compile_definitions(
    protocol-test
    PRIVATE USER_MAX_REMOTE_MEMORY=${INFINISWAP_MAX_REMOTE_MEMORY_GB})
  target_compile_options(protocol-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(protocol-test PRIVATE infiniswap-protocol)
  add_test(NAME protocol COMMAND protocol-test)

  add_executable(
    protocol-previous-build-test
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/infiniswap_protocol.c
    tests/protocol_previous_build_test.c)
  target_compile_definitions(
    protocol-previous-build-test
    PRIVATE
      IS_PROTOCOL_MINOR_CURRENT=0
      USER_MAX_REMOTE_MEMORY=${INFINISWAP_MAX_REMOTE_MEMORY_GB})
  target_compile_options(
    protocol-previous-build-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(
    protocol-previous-build-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
  add_test(NAME protocol-previous-build COMMAND protocol-previous-build-test)

  add_executable(rdma-common-test tests/rdma_common_test.c)
  target_compile_options(rdma-common-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(rdma-common-test PRIVATE infiniswap-provider-core)
  add_test(NAME rdma-common COMMAND rdma-common-test)
endif()
