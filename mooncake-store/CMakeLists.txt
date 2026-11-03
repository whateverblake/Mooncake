project(MooncakeStore VERSION 2.0.0)

set(MOONCAKE_STORE_INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Extract version components for C++ usage
set(MOONCAKE_STORE_VERSION ${PROJECT_VERSION})

# Read display version from mooncake-wheel/pyproject.toml
set(PYPROJECT_TOML_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../mooncake-wheel/pyproject.toml")
if(EXISTS "${PYPROJECT_TOML_PATH}")
    file(READ "${PYPROJECT_TOML_PATH}" PYPROJECT_CONTENT)
    string(REGEX MATCH "version = \"([^\"]+)\"" _ "${PYPROJECT_CONTENT}")
    set(MOONCAKE_DISPLAY_VERSION_BASE "${CMAKE_MATCH_1}")
endif()
if(NOT MOONCAKE_DISPLAY_VERSION_BASE)
    message(WARNING "Could not read display version from ${PYPROJECT_TOML_PATH}; falling back to MOONCAKE_STORE_VERSION")
    set(MOONCAKE_DISPLAY_VERSION_BASE "${MOONCAKE_STORE_VERSION}")
endif()

# Get short git commit hash for display
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_SHORT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT GIT_SHORT_HASH)
    set(GIT_SHORT_HASH "unknown")
endif()
set(MOONCAKE_DISPLAY_VERSION "${MOONCAKE_DISPLAY_VERSION_BASE} (git: ${GIT_SHORT_HASH})")

configure_file(include/version.h.in include/version.h @ONLY)

if (STORE_USE_ETCD)
    set(ETCD_WRAPPER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/../mooncake-common/etcd/)
    set(ETCD_WRAPPER_LIB ${CMAKE_CURRENT_BINARY_DIR}/../mooncake-common/etcd/libetcd_wrapper.so)
endif()

if (STORE_USE_K8S_LEASE)
    set(K8S_LEASE_WRAPPER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/../mooncake-common/k8s-lease/)
    set(K8S_LEASE_WRAPPER_LIB ${CMAKE_CURRENT_BINARY_DIR}/../mooncake-common/k8s-lease/libk8s_lease_wrapper.so)
endif()

if(TARGET Mooncake::liburing)
  message(STATUS "io_uring: Enabled for mooncake_store")
else()
  message(STATUS "io_uring: Disabled (liburing not found)")
endif()

if(STORE_USE_REDIS)
  message(STATUS "Redis HA backend: Enabled")
endif()

if (NOT STORE_USE_ETCD AND NOT STORE_USE_REDIS AND NOT STORE_USE_K8S_LEASE)
    message(STATUS "Store HA backends are disabled")
endif()

# Add subdirectories
add_subdirectory(src)

if (STORE_USE_ETCD)
    add_subdirectory(tools)
endif()

if (BUILD_UNIT_TESTS)
    add_subdirectory(tests)
endif()

if (BUILD_BENCHMARK)
    add_subdirectory(benchmarks)
endif()

if(BUILD_UBSMEM_TESTS)
  add_executable(ubsmem_store_test tests/ubsmem_store_test.cpp)
  target_link_libraries(ubsmem_store_test PRIVATE mooncake_store)
  add_executable(ubsmem_allocator_test tests/ubsmem_allocator_test.cpp)
  target_link_libraries(ubsmem_allocator_test PRIVATE mooncake_store
                                                      cachelib_memory_allocator)
  add_test(NAME ubsmem_allocator COMMAND ubsmem_allocator_test)
  add_executable(ubsmem_ownership_test tests/ubsmem_ownership_test.cpp)
  target_link_libraries(ubsmem_ownership_test PRIVATE mooncake_store)

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME ubsmem_store_ownership_failure
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ubsmem_store.py
      $<TARGET_FILE:mooncake_master> $<TARGET_FILE:ubsmem_ownership_test>
      --ownership-failure)
  set_tests_properties(ubsmem_store_ownership_failure PROPERTIES TIMEOUT 60)

  add_test(
    NAME ubsmem_store_three_process
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ubsmem_store.py
      $<TARGET_FILE:mooncake_master> $<TARGET_FILE:ubsmem_store_test>)
  set_tests_properties(ubsmem_store_three_process PROPERTIES TIMEOUT 120)
  if(USE_TCP)
    add_test(
      NAME ubsmem_store_tcp_regression
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ubsmem_store.py
        $<TARGET_FILE:mooncake_master> $<TARGET_FILE:ubsmem_store_test> --tcp)
    set_tests_properties(ubsmem_store_tcp_regression PROPERTIES TIMEOUT 120)
  endif()
  add_test(
    NAME ubsmem_store_master_policy
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ubsmem_store.py
      $<TARGET_FILE:mooncake_master> $<TARGET_FILE:ubsmem_store_test>
      --reject-policy)
  set_tests_properties(ubsmem_store_master_policy PROPERTIES TIMEOUT 60)
endif()
