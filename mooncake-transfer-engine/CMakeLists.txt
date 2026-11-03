cmake_minimum_required(VERSION 3.16)
project(mooncake-transfer-engine)

if(BUILD_UBSMEM_TESTS)
  enable_testing()
endif()

include(CheckCXXSourceCompiles)

if (NOT GLOBAL_CONFIG)
  if (USE_ETCD)
    message(FATAL_ERROR "Cannot enable USE_ETCD while building transfer engine independently")
  endif()
  include(../mooncake-common/FindJsonCpp.cmake)
  include(../mooncake-common/FindGLOG.cmake)
  include(../mooncake-common/common.cmake)
  include(../cmake/Dependencies.cmake)

  if(USE_REDIS)
    mooncake_provide_hiredis(REQUIRED)
  elseif(USE_TENT)
    mooncake_provide_hiredis()
  endif()
  if(USE_TENT)
    mooncake_provide_liburing()
  endif()

  add_subdirectory(../extern/pybind11
                   ${CMAKE_CURRENT_BINARY_DIR}/extern/pybind11)

  # Build asio_shared in mooncake-common for standalone builds.
  # Set compile definitions before adding targets that include ASIO headers.
  add_compile_definitions(ASIO_SEPARATE_COMPILATION ASIO_DYN_LINK)
  add_subdirectory(../mooncake-common/src
                   ${CMAKE_CURRENT_BINARY_DIR}/mooncake-common-src)
endif() # GLOBAL_CONFIG

if (USE_CUDA)
  set(CMAKE_REQUIRED_LIBRARIES mlx5)
  check_cxx_source_compiles(
    "#include <infiniband/mlx5dv.h>
     #include <infiniband/verbs.h>
     int main() {
       mlx5dv_devx_umem_in input{};
       input.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
       input.dmabuf_fd = -1;
       auto reg_ex = &mlx5dv_devx_umem_reg_ex;
       (void)reg_ex;
       return 0;
     }"
    MOONCAKE_HAVE_MLX5_DMABUF_UMEM)
  unset(CMAKE_REQUIRED_LIBRARIES)

  if (MOONCAKE_HAVE_MLX5_DMABUF_UMEM)
    add_compile_definitions(MOONCAKE_HAVE_MLX5_DMABUF_UMEM)
    message(STATUS "CUDA IBGDA DMA-BUF DevX UMEM support enabled")
  else()
    message(WARNING
      "CUDA IBGDA DMA-BUF DevX UMEM support disabled: the installed mlx5 headers or libmlx5 do not provide MLX5DV_UMEM_MASK_DMABUF, mlx5dv_devx_umem_in::dmabuf_fd, and mlx5dv_devx_umem_reg_ex. Update rdma-core/libmlx5 to enable DMA-BUF control memory; Device API will use GPU-VA and host-mapped fallback.")
  endif()
endif()
if (USE_ASCEND)
  include(../mooncake-common/FindMpi.cmake)
  file(GLOB ASCEND_TOOLKIT_ROOT "/usr/local/Ascend/ascend-toolkit/latest/*-linux")
  set(ASCEND_INCLUDE_DIR "${ASCEND_TOOLKIT_ROOT}/include")

  include_directories(/usr/local/include
                      /usr/include
                      ${ASCEND_INCLUDE_DIR}
                      ${ASCEND_INCLUDE_DIR}/hccl
                      ${ASCEND_INCLUDE_DIR}/experiment
                      ${ASCEND_INCLUDE_DIR}/experiment/hccl
                      ${ASCEND_INCLUDE_DIR}/experiment/slog/toolchain
                      ${ASCEND_INCLUDE_DIR}/experiment/metadef/common/util/error_manager
                      ${ASCEND_INCLUDE_DIR}/experiment/runtime
                      ${ASCEND_INCLUDE_DIR}/experiment/msprof
                      ${MPI_INCLUDE_DIR}
                      /usr/local/Ascend/ascend-toolkit/latest/tools/hccl_test/common/src/
                      )
endif()

if (USE_ASCEND_HETEROGENEOUS)
  file(GLOB ASCEND_TOOLKIT_ROOT "/usr/local/Ascend/ascend-toolkit/latest/*-linux")
  set(ASCEND_INCLUDE_DIR "${ASCEND_TOOLKIT_ROOT}/include")
  include_directories(/usr/local/include
                      /usr/include
                      ${ASCEND_INCLUDE_DIR})
endif()

include_directories(../mooncake-common/include)
include_directories(include)
add_subdirectory(include)
add_subdirectory(src)

if (BUILD_UNIT_TESTS)
  add_subdirectory(tests)
endif()

if (BUILD_EXAMPLES)
  add_subdirectory(example)
endif()

if (WITH_RUST_EXAMPLE)
  add_subdirectory(rust)
endif()

# Non-peermem requires cuMem allocated memory pointers; since WITH_NVIDIA_PEERMEM
# is now a runtime environment variable, always build when GPU support is present.
if (USE_MNNVL OR USE_CUDA OR USE_HIP OR USE_MUSA OR USE_MACA OR USE_SUPA)
  add_subdirectory(nvlink-allocator)
endif()

# Build ubshmem allocator for Huawei NPU fabric memory
if (USE_UBSHMEM)
  add_subdirectory(ubshmem-allocator)
endif()

if (USE_TENT)
  add_subdirectory(tent)
endif()

# tebench only needs the classic Transfer Engine for its default "classic"
# backend; the TENT backend is compiled in conditionally (see benchmark/
# CMakeLists.txt and main.cpp). Keep it gated solely on BUILD_BENCHMARK so it
# can be built without enabling USE_TENT.
if (BUILD_BENCHMARK)
  add_subdirectory(benchmark)
endif()

if(BUILD_UBSMEM_TESTS)
  if(NOT USE_UBSMEM)
    message(FATAL_ERROR "BUILD_UBSMEM_TESTS requires USE_UBSMEM")
  endif()
  # These tests use the mock's session files and locks directly. The Store
  # tests in mooncake-store also run with the SDK backend.
  if(UBSMEM_BACKEND STREQUAL "mock")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(ubsmem_memory_test tests/ubsmem_memory_test.cpp)
    target_link_libraries(ubsmem_memory_test PRIVATE ubsmem_memory)
    add_test(NAME ubsmem_memory COMMAND ubsmem_memory_test)
    add_executable(ubsmem_transport_test tests/ubsmem_transport_test.cpp)
    target_link_libraries(ubsmem_transport_test PRIVATE transfer_engine
                                                        ubsmem_memory)
    add_test(NAME ubsmem_transport COMMAND ubsmem_transport_test)
    if(USE_TCP AND NOT USE_EVENT_DRIVEN_COMPLETION)
      add_test(NAME ubsmem_tcp_regression COMMAND ubsmem_transport_test tcp)
      set_tests_properties(ubsmem_tcp_regression PROPERTIES TIMEOUT 30)
    endif()
    if(USE_HTTP)
      add_test(
        NAME ubsmem_metadata
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ubsmem_metadata.py
          $<TARGET_FILE:ubsmem_transport_test>)
      set_tests_properties(ubsmem_metadata PROPERTIES TIMEOUT 60)
    endif()
    set_tests_properties(ubsmem_memory ubsmem_transport PROPERTIES TIMEOUT 30)
  endif()
endif()
