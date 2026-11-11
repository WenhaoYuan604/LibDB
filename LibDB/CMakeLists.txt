cmake_minimum_required(VERSION 3.13)
project(test)







set(CMAKE_BUILD_TYPE Release)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2")






set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-as-needed")
if(DEFINED ENV{SPDK_ROOT} AND NOT "$ENV{SPDK_ROOT}" STREQUAL "")
    set(_SPDK_ROOT_DEFAULT "$ENV{SPDK_ROOT}")
else()
    get_filename_component(_SPDK_ROOT_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../../spdk" ABSOLUTE)
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



add_executable(libdb space_stats.c main.c base.c config.c client.c client_workload.c dram_index.c garbadge_c.c get.c hist.c index.c insert.c kvbuffer.c kvcache.c leaf_node_file.c
merge_flush.c nvm_index.c nvme_pipe.c pmdk.c recovery.c scan.c server.c spdk_struct.c subspace.c task.c write_buffer_index.c ${SPDK_ROOT}/lib/nvme/nvme_pcie.c)








target_link_libraries(libdb ${SPDK_LIBRARIES} -lpthread -lm -luuid -lrt  -lpmemobj -lpmem -lnuma )


if(SPDK_SYS_LIBS_FOUND)
    target_link_libraries(libdb ${SPDK_SYS_LIBS_LIBRARIES})
endif()


set_target_properties(libdb PROPERTIES
    OUTPUT_NAME test
    INTERPROCEDURAL_OPTIMIZATION TRUE
    LINK_FLAGS "-Wl,--whole-archive ${SPDK_SYS_LIBS_LIBRARIES} -Wl,--no-whole-archive"
)
