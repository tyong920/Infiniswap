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

set(INFINISWAP_MAX_CLIENT 32 CACHE STRING
    "Maximum number of Memory Consumers connected to one Memory Provider")
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

add_library(infiniswap-provider-core STATIC rdma-common.c)
target_compile_features(infiniswap-provider-core PUBLIC c_std_11)
target_compile_definitions(
  infiniswap-provider-core
  PUBLIC
    _POSIX_C_SOURCE=200112L
    USER_MAX_CLIENT=${INFINISWAP_MAX_CLIENT}
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
    Threads::Threads)

add_executable(infiniswap-daemon infiniswap-daemon.c)
target_compile_options(infiniswap-daemon PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(infiniswap-daemon PRIVATE infiniswap-provider-core)

install(TARGETS infiniswap-daemon RUNTIME DESTINATION sbin)

if(BUILD_TESTING)
  add_executable(rdma-common-test tests/rdma_common_test.c)
  target_compile_options(rdma-common-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(rdma-common-test PRIVATE infiniswap-provider-core)
  add_test(NAME rdma-common COMMAND rdma-common-test)
endif()
