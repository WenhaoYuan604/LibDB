cmake_minimum_required(VERSION 3.13)
project(test)
# Empty preserves config.h defaults; each experiment gets a separate build tree.
foreach(setting LIBDB_SUBSPACE_MAX_KEYS LIBDB_FLUSH_BLOCK_COUNT)
    set(${setting} "" CACHE STRING "Optional positive compile-time override")
    if(NOT "${${setting}}" STREQUAL "")
        if(NOT "${${setting}}" MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "${setting} must be a positive integer")
        endif()
        add_compile_definitions(${setting}=${${setting}})
    endif()
endforeach()
foreach(setting LIBDB_SUBSPACE_REDUNDANT_THRESHOLD LIBDB_GC_SPACE_LIMIT)
    set(${setting} "" CACHE STRING "Optional positive decimal GC configuration")
    if(NOT "${${setting}}" STREQUAL "")
        if(NOT "${${setting}}" MATCHES "^[0-9]+([.][0-9]+)?$" OR NOT ${setting} GREATER 0)
            message(FATAL_ERROR "${setting} must be a positive decimal")
        endif()
        add_compile_definitions(${setting}=${${setting}})
    endif()
endforeach()






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



add_executable(libdb space_stats.c phase_io.c main.c base.c config.c client.c dram_index.c garbadge_c.c get.c hist.c index.c insert.c kvbuffer.c kvcache.c
wal.c merge_flush.c slot_index.c nvme_pipe.c  recovery.c scan.c server.c spdk_struct.c subspace.c task.c write_buffer_index.c ${SPDK_ROOT}/lib/nvme/nvme_pcie.c)

# Optional write-buffer overrides for the main program.
foreach(setting LIBDB_WBF_CAPACITY_LIMIT_BYTES LIBDB_NVM_FLUSH_HIGH_BYTES
                LIBDB_NVM_FLUSH_LOW_BYTES LIBDB_NVM_FLUSH_POLL_NS)
    set(${setting} "" CACHE STRING "Optional positive uint64 write-buffer setting (libdb only)")
    if(NOT "${${setting}}" STREQUAL "")
        if(NOT "${${setting}}" MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "${setting} must be a positive integer")
        endif()
        string(LENGTH "${${setting}}" _nvm_digits)
        if(_nvm_digits GREATER 20 OR (_nvm_digits EQUAL 20 AND
           "${${setting}}" STRGREATER "18446744073709551615"))
            message(FATAL_ERROR "${setting} exceeds uint64")
        endif()
        target_compile_definitions(libdb PRIVATE ${setting}=${${setting}}ULL)
    endif()
endforeach()
set(LIBDB_DYNAMIC_FLUSH_ENABLED "" CACHE STRING "Optional 0/1 dynamic Flush override (libdb only)")
if(NOT "${LIBDB_DYNAMIC_FLUSH_ENABLED}" STREQUAL "")
    if(NOT "${LIBDB_DYNAMIC_FLUSH_ENABLED}" MATCHES "^[01]$")
        message(FATAL_ERROR "LIBDB_DYNAMIC_FLUSH_ENABLED must be 0 or 1")
    endif()
    target_compile_definitions(libdb PRIVATE LIBDB_DYNAMIC_FLUSH_ENABLED=${LIBDB_DYNAMIC_FLUSH_ENABLED})
endif()









find_path(LIBDB_DPDK_LAUNCH_INCLUDE rte_launch.h HINTS
    "${SPDK_ROOT}/dpdk/lib/eal/include" "${SPDK_ROOT}/dpdkbuild/include"
    "${SPDK_ROOT}/dpdk/build/include" PATH_SUFFIXES dpdk)
if(NOT LIBDB_DPDK_LAUNCH_INCLUDE)
    message(FATAL_ERROR "rte_launch.h is required by the SPDK DPDK injector lifecycle")
endif()
target_include_directories(libdb PRIVATE ${LIBDB_DPDK_LAUNCH_INCLUDE})
target_link_libraries(libdb ${SPDK_LIBRARIES} -lpthread -lm -luuid -lrt  -lnuma )


if(SPDK_SYS_LIBS_FOUND)
    target_link_libraries(libdb ${SPDK_SYS_LIBS_LIBRARIES})
endif()


set_target_properties(libdb PROPERTIES
    OUTPUT_NAME test
    INTERPROCEDURAL_OPTIMIZATION TRUE
    LINK_FLAGS "-Wl,--whole-archive ${SPDK_SYS_LIBS_LIBRARIES} -Wl,--no-whole-archive"
)

