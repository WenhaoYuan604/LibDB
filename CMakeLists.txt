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

add_executable(test main.c base.c config.c client.c dram_index.c garbadge_c.c get.c hist.c index.c insert.c kvbuffer.c kvcache.c leaf_node_file.c
merge_flush.c nvm_index.c nvme_pipe.c pmdk.c recovery.c scan.c server.c spdk_struct.c subspace.c task.c write_buffer_index.c ${SPDK_ROOT}/lib/nvme/nvme_pcie.c)

# # 链接 SPDK 和 DPDK 库
# file(GLOB LIBS "${SPDK_ROOT}/dpdk/build/lib/lib*.so")
# message(STATUS "LIBS: ${LIBS}")

# target_link_libraries(test ${SPDK_LIBRARIES} -lpthread -lm -luuid -lrt  -lpmemobj -lpmem ${LIBS})
# 在link_directories的目录下(也许还有默认目录)链接所需要的.so文件
# target_link_libraries(test ${SPDK_LIBRARIES} -lpthread -lm -luuid -lrt  -lpmemobj -lpmem -lnuma -ltcmalloc)
target_link_libraries(test ${SPDK_LIBRARIES} -lpthread -lm -luuid -lrt  -lpmemobj -lpmem -lnuma )

# 对于静态库链接，可能需要额外的设置
if(SPDK_SYS_LIBS_FOUND)
    target_link_libraries(test ${SPDK_SYS_LIBS_LIBRARIES})
endif()

# 处理构造函数（如果使用静态库）
set_target_properties(test PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE
    LINK_FLAGS "-Wl,--whole-archive ${SPDK_SYS_LIBS_LIBRARIES} -Wl,--no-whole-archive"
)
