cmake_minimum_required(VERSION 3.13)
project(test)


# set(CMAKE_BUILD_TYPE Debug)
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")


set(CMAKE_BUILD_TYPE Release)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2")

# set(CMAKE_BUILD_TYPE Release)
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")


set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-as-needed")
if(DEFINED ENV{SPDK_ROOT} AND NOT "$ENV{SPDK_ROOT}" STREQUAL "")
    set(_SPDK_ROOT_DEFAULT "$ENV{SPDK_ROOT}")
else()
    get_filename_component(_SPDK_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../spdk" ABSOLUTE)
endif()
set(SPDK_ROOT "${_SPDK_ROOT_DEFAULT}" CACHE PATH "Path to the SPDK source tree")
set(SPDK_PKG_CONFIG_DIR "${SPDK_ROOT}/build/lib/pkgconfig" CACHE PATH "Path to SPDK pkg-config files")

if(NOT EXISTS "${SPDK_ROOT}/include/spdk/nvme.h" OR
   NOT EXISTS "${SPDK_ROOT}/lib/nvme/nvme_pcie.c")
    message(FATAL_ERROR "SPDK was not found under '${SPDK_ROOT}'. Set SPDK_ROOT or pass -DSPDK_ROOT=/path/to/spdk.")
endif()

if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    set(ENV{PKG_CONFIG_PATH} "${SPDK_PKG_CONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
else()
    set(ENV{PKG_CONFIG_PATH} "${SPDK_PKG_CONFIG_DIR}")
endif()

message(STATUS "SPDK_ROOT: ${SPDK_ROOT}")
message(STATUS "SPDK pkg-config directory: ${SPDK_PKG_CONFIG_DIR}")

# find_package的部分作用是初始化变量如SPDK_LIBRARY_DIRS、SPDK_INCLUDE_DIRS、SPDK_LIBRARIES，若没有找到，则变量都为空
find_package(PkgConfig REQUIRED)
message(STATUS "PkgConfig version: ${PkgConfig_VERSION}")

pkg_check_modules(SPDK REQUIRED spdk_nvme spdk_env_dpdk)
pkg_check_modules(SPDK_SYS_LIBS REQUIRED spdk_syslibs)

message(STATUS "SPDK_LIBRARY_DIRS: ${SPDK_LIBRARY_DIRS}")
message(STATUS "SPDK_INCLUDE_DIRS: ${SPDK_INCLUDE_DIRS}")
message(STATUS "SPDK_LIBRARIES: ${SPDK_LIBRARIES}")

include_directories(${SPDK_INCLUDE_DIRS} "${SPDK_ROOT}/include")
link_directories(${SPDK_LIBRARY_DIRS})
link_directories("${SPDK_ROOT}/build/lib")
link_directories("${SPDK_ROOT}/dpdk/build/lib")

message(STATUS "SPDK_LIBRARY_DIRS: ${SPDK_LIBRARY_DIRS}")
message(STATUS "SPDK_INCLUDE_DIRS: ${SPDK_INCLUDE_DIRS}")
message(STATUS "SPDK_LIBRARIES: ${SPDK_LIBRARIES}")

# add_executable(test main.c base.c client.c db.c index.c  kvbuffer.c kvcache.c nvme_pipe.c pmdk.c server.c spdk_struct.c spdk_poller.c subspace.c ../spdk/lib/nvme/nvme_pcie.c)

add_executable(libdb space_stats.c main.c base.c config.c client.c dram_index.c garbadge_c.c get.c hist.c index.c insert.c kvbuffer.c kvcache.c leaf_node_file.c
merge_flush.c nvm_index.c nvme_pipe.c pmdk.c recovery.c scan.c server.c spdk_struct.c subspace.c task.c write_buffer_index.c ${SPDK_ROOT}/lib/nvme/nvme_pcie.c)

# # 链接 SPDK 和 DPDK 库
# file(GLOB LIBS "${SPDK_ROOT}/dpdk/build/lib/lib*.so")
# message(STATUS "LIBS: ${LIBS}")

# target_link_libraries(test ${SPDK_LIBRARIES} -lpthread -lm -luuid -lrt  -lpmemobj -lpmem ${LIBS})
# 在link_directories的目录下(也许还有默认目录)链接所需要的.so文件
# target_link_libraries(test ${SPDK_LIBRARIES} -lpthread -lm -luuid -lrt  -lpmemobj -lpmem -lnuma -ltcmalloc)
target_link_libraries(libdb ${SPDK_LIBRARIES} -lpthread -lm -luuid -lrt  -lpmemobj -lpmem -lnuma )

# 对于静态库链接，可能需要额外的设置
if(SPDK_SYS_LIBS_FOUND)
    target_link_libraries(libdb ${SPDK_SYS_LIBS_LIBRARIES})
endif()

# 处理构造函数（如果使用静态库）
set_target_properties(libdb PROPERTIES
    OUTPUT_NAME test
    INTERPROCEDURAL_OPTIMIZATION TRUE
    LINK_FLAGS "-Wl,--whole-archive ${SPDK_SYS_LIBS_LIBRARIES} -Wl,--no-whole-archive"
)

# Tests use memory-backed NVMe and ordinary temporary PMDK files, never a device.
include(CTest)
if(BUILD_TESTING)
    foreach(mode IN ITEMS enabled disabled)
        add_executable(nvm_control_${mode}_test tests/nvm_control_test.c space_stats.c)
        target_include_directories(nvm_control_${mode}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(nvm_control_${mode}_test PRIVATE -fcommon -ffunction-sections -fdata-sections -UNDEBUG)
        target_compile_definitions(nvm_control_${mode}_test PRIVATE LIBDB_NVM_FLUSH_LIMIT_BYTES=65536)
        if(mode STREQUAL "disabled")
            target_compile_definitions(nvm_control_${mode}_test PRIVATE LIBDB_DYNAMIC_FLUSH_ENABLED=0)
        endif()
        target_link_options(nvm_control_${mode}_test PRIVATE -Wl,--gc-sections)
        target_link_libraries(nvm_control_${mode}_test PRIVATE pthread)
        add_test(NAME nvm_control_${mode} COMMAND nvm_control_${mode}_test)
    endforeach()
    add_executable(wbf_occupancy_test tests/wbf_occupancy_test.c kvbuffer.c config.c)
    target_include_directories(wbf_occupancy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(wbf_occupancy_test PRIVATE -fcommon -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(wbf_occupancy_test PRIVATE -Wl,--gc-sections -Wl,--wrap=malloc)
    target_link_libraries(wbf_occupancy_test PRIVATE pmemobj pmem pthread)
    add_test(NAME wbf_occupancy COMMAND wbf_occupancy_test)
    set_tests_properties(wbf_occupancy PROPERTIES TIMEOUT 30)
    add_executable(merge_test tests/merge_test.c space_stats.c dram_index.c subspace.c
        write_buffer_index.c kvcache.c nvm_index.c merge_flush.c task.c get.c
        scan.c insert.c index.c garbadge_c.c leaf_node_file.c recovery.c)
    target_compile_definitions(merge_test PRIVATE LIBDB_NVM_FLUSH_LIMIT_BYTES=65536)
    target_include_directories(merge_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(merge_test PRIVATE -fcommon -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(merge_test PRIVATE -Wl,--gc-sections
        -Wl,--wrap=nvm_index_get_free_node -Wl,--wrap=nvm_index_reserve_free_nodes
        -Wl,--wrap=nvm_index_commit_slot_updates)
    target_link_libraries(merge_test PRIVATE pmemobj pmem pthread m)
    option(LIBDB_TEST_SANITIZERS "Enable ASan/UBSan for merge tests" OFF)
    if(LIBDB_TEST_SANITIZERS)
        target_compile_options(merge_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(merge_test PRIVATE -fsanitize=address,undefined)
    endif()
    add_executable(gc_poll_test tests/gc_poll_test.c space_stats.c)
    target_include_directories(gc_poll_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(gc_poll_test PRIVATE -fcommon -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(gc_poll_test PRIVATE -Wl,--gc-sections
        -Wl,--wrap=clock_gettime -Wl,--wrap=nanosleep)
    target_link_libraries(gc_poll_test PRIVATE pthread m)
    add_test(NAME gc_poll COMMAND gc_poll_test)
    set_tests_properties(gc_poll PROPERTIES TIMEOUT 15)
    add_executable(server_shutdown_test tests/server_shutdown_test.c server.c task.c space_stats.c)
    target_include_directories(server_shutdown_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(server_shutdown_test PRIVATE -fcommon -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(server_shutdown_test PRIVATE -Wl,--gc-sections)
    target_link_libraries(server_shutdown_test PRIVATE pthread)
    add_test(NAME server_shutdown COMMAND server_shutdown_test)
    set_tests_properties(server_shutdown PROPERTIES TIMEOUT 10)
    add_executable(space_stats_test tests/space_stats_test.c space_stats.c spdk_struct.c)
    target_include_directories(space_stats_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(space_stats_test PRIVATE -fcommon -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(space_stats_test PRIVATE -Wl,--gc-sections)
    target_link_libraries(space_stats_test PRIVATE pthread)
    add_test(NAME space_stats COMMAND space_stats_test)
    add_executable(allocator_test tests/allocator_test.c spdk_struct.c)
    target_include_directories(allocator_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(allocator_test PRIVATE -fcommon -ffunction-sections -fdata-sections -UNDEBUG)
    target_link_options(allocator_test PRIVATE -Wl,--gc-sections)
    target_link_libraries(allocator_test PRIVATE pthread)
    add_test(NAME allocator COMMAND allocator_test)
    set_tests_properties(allocator PROPERTIES TIMEOUT 10)
    add_test(NAME merge COMMAND merge_test)
    set_tests_properties(merge PROPERTIES TIMEOUT 90)
endif()
