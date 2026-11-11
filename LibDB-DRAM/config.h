#ifndef LIBDB_CONFIG_H_
#define LIBDB_CONFIG_H_

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

/*
 * LibDB compile-time configuration
 * --------------------------------
 * These values define slot layout, fixed-size structures, or
 * low-level I/O geometry. Change them here, rebuild the whole project, and
 * start with fresh persistent data. Data created with different values is not
 * guaranteed to be compatible.
 */

/* NVMe geometry and queue limits. */
#define LIBDB_BLOCK_SIZE                         4096U
#define LIBDB_PHYSICAL_BLOCKS_PER_LOGICAL_BLOCK 1U
#define LIBDB_MAX_NVME_QPAIRS                   31
#define LIBDB_SPDK_IO_QUEUE_SIZE                1024U
#define LIBDB_SPDK_IO_QUEUE_REQUESTS            1024U

/* Flush, subspace, slot, and free-space group layout. */
#ifndef LIBDB_FLUSH_BLOCK_COUNT
#define LIBDB_FLUSH_BLOCK_COUNT                 4
#endif
#if LIBDB_FLUSH_BLOCK_COUNT <= 0
#error "LIBDB_FLUSH_BLOCK_COUNT must be a positive integer"
#endif
/* Local trigger thresholds, derived from the same block count in 64 bits. */
#define LIBDB_FLUSH_THRESHOLD_BYTES (1ULL * LIBDB_BLOCK_SIZE * LIBDB_FLUSH_BLOCK_COUNT)
#define LIBDB_FLUSH_HARD_THRESHOLD_BYTES (2ULL * LIBDB_FLUSH_THRESHOLD_BYTES)
#define LIBDB_INDEX_SLOT_SIZE                     256U
#ifndef LIBDB_SUBSPACE_MAX_KEYS
#define LIBDB_SUBSPACE_MAX_KEYS                 128U
#endif
/* Match clean: fixed DMA capacity, including builds with larger groups. */
#define LIBDB_TASK_BUFFER_BLOCK_COUNT           256U
#ifndef LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD
#define LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD (LIBDB_SUBSPACE_MAX_KEYS / 2U)
#endif
_Static_assert(LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD > 0 &&
               LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD <= LIBDB_SUBSPACE_MAX_KEYS,
               "invalid group merge threshold");
/* A slot stores one boundary key followed by key-to-SSD-address entries. */
#define LIBDB_INDEX_SLOT_ENTRY_CAPACITY           \
    ((LIBDB_INDEX_SLOT_SIZE - sizeof(key_type)) / sizeof(struct slot_element))
#define LIBDB_INDEX_SLOT_PADDING_BYTES             \
    (LIBDB_INDEX_SLOT_SIZE - sizeof(key_type) - \
     LIBDB_INDEX_SLOT_ENTRY_CAPACITY * sizeof(struct slot_element))
/* Reserve enough whole slots for twice the maximum subspace key count. */
#define LIBDB_SUBSPACE_MAX_SLOTS                \
    (((2U * LIBDB_SUBSPACE_MAX_KEYS) + LIBDB_INDEX_SLOT_ENTRY_CAPACITY - 1U) / \
     LIBDB_INDEX_SLOT_ENTRY_CAPACITY)
/* Redundant bytes / valid bytes: 0.3 targets occupied / valid = 1.3.
 * Comparisons against occupied bytes must use the converted fraction. */
#ifndef LIBDB_SUBSPACE_REDUNDANT_THRESHOLD
#define LIBDB_SUBSPACE_REDUNDANT_THRESHOLD      0.3
#endif
#define LIBDB_SUBSPACE_REDUNDANT_FRACTION       \
    (LIBDB_SUBSPACE_REDUNDANT_THRESHOLD / (1.0 + LIBDB_SUBSPACE_REDUNDANT_THRESHOLD))
#define LIBDB_GC_RELAXED_THRESHOLD             0.55
#ifndef LIBDB_GC_SPACE_LIMIT
#define LIBDB_GC_SPACE_LIMIT                   1.28
#endif
#define LIBDB_GC_POLL_NS                       UINT64_C(100000000)
/* Workload-only hysteresis over rounded slot bytes; sampled capacity in all phases. */
#ifndef LIBDB_DYNAMIC_FLUSH_ENABLED
#define LIBDB_DYNAMIC_FLUSH_ENABLED 1
#endif
#ifndef LIBDB_WBF_CAPACITY_LIMIT_BYTES
#define LIBDB_WBF_CAPACITY_LIMIT_BYTES (UINT64_C(16) * 1024 * 1024 * 1024)
#endif
#ifndef LIBDB_NVM_FLUSH_HIGH_BYTES
#define LIBDB_NVM_FLUSH_HIGH_BYTES (UINT64_C(14) * 1024 * 1024 * 1024)
#endif
#ifndef LIBDB_NVM_FLUSH_LOW_BYTES
#define LIBDB_NVM_FLUSH_LOW_BYTES (UINT64_C(13) * 1024 * 1024 * 1024 + UINT64_C(512) * 1024 * 1024)
#endif
#ifndef LIBDB_NVM_FLUSH_POLL_NS
#define LIBDB_NVM_FLUSH_POLL_NS UINT64_C(100000000)
#endif
_Static_assert(LIBDB_NVM_FLUSH_LOW_BYTES < LIBDB_NVM_FLUSH_HIGH_BYTES &&
               LIBDB_NVM_FLUSH_HIGH_BYTES < LIBDB_WBF_CAPACITY_LIMIT_BYTES,
               "require NVM low watermark < high watermark < sampled capacity limit");
_Static_assert(LIBDB_NVM_FLUSH_POLL_NS > 0, "NVM poll interval must be positive");

#define LIBDB_FREE_SPACE_GROUP_BLOCKS           1024

/* Largest supported logical write-buffer record (header included). */
#define LIBDB_WBF_MAX_RECORD_SIZE               32768U


#define LIBDB_VALID_VALUE_PREFIX_SIZE             32U

/* Index, cache, and GC limits. */
#define LIBDB_DRAM_INDEX_FANOUT                 128
#define LIBDB_KVCACHE_MAX_LEVEL                 3
#define LIBDB_WRITE_BUFFER_INDEX_LEVELS         3
#define LIBDB_WRITE_BUFFER_INDEX_RESERVE_LEVELS 3
#define LIBDB_GC_BLOCK_CHOOSE_PERCENT           0.5
#define LIBDB_MAX_PENDING_SCANS                 10000
#define LIBDB_MAX_THREADS                       128

/* Defaults that can be overridden at runtime. */
#define LIBDB_DEFAULT_BOUNDARY_SIZE             1000
#define LIBDB_DEFAULT_LOAD_INJECTORS            8
#define LIBDB_DEFAULT_TASKS_PER_THREAD          16

/* Runtime configuration is supplied by CLI options or LIBDB_* environment variables. */
#define LIBDB_PCI_ADDR_CAPACITY                 32
#define LIBDB_PATH_CAPACITY                     4096

struct libdb_runtime_config {
    char pci_addr[LIBDB_PCI_ADDR_CAPACITY];
    char warm_path[LIBDB_PATH_CAPACITY];
    char test_path[LIBDB_PATH_CAPACITY];
};

extern struct libdb_runtime_config g_libdb_config;

int libdb_config_init_from_env(void);
int libdb_config_set_pci_addr(const char *value);
int libdb_config_set_pmem_dir(const char *value);
int libdb_config_set_warm_path(const char *value);
int libdb_config_set_test_path(const char *value);
int libdb_config_validate(char *error, size_t error_size);

int libdb_config_validate_inputs(char *error, size_t error_size, int require_test);

/*
 * Compatibility aliases for the current codebase. New code should prefer the
 * LIBDB_* names above. Keeping aliases here makes this the only definition site.
 */
#define BLOCK_SIZE                           LIBDB_BLOCK_SIZE
#define physical_block_to_logical_block      LIBDB_PHYSICAL_BLOCKS_PER_LOGICAL_BLOCK
#define ctr_max_qp                           LIBDB_MAX_NVME_QPAIRS
#define Flush_Blcok_Num                      LIBDB_FLUSH_BLOCK_COUNT
#define SUBSPACE_MAX_KEY                     LIBDB_SUBSPACE_MAX_KEYS
#define INDEX_SLOT_SIZE                    LIBDB_INDEX_SLOT_SIZE
#define SUBSPACE_MAX_SLOT                    LIBDB_SUBSPACE_MAX_SLOTS
#define SUBSPACE_REDUNDANT_PERCENT_THRESHOLD LIBDB_SUBSPACE_REDUNDANT_THRESHOLD
#define alloc_grain                          LIBDB_FREE_SPACE_GROUP_BLOCKS
#define DRAM_INDEX_MAX_NODE                  LIBDB_DRAM_INDEX_FANOUT
#define MAX_LEVEL                            LIBDB_KVCACHE_MAX_LEVEL
#define SKIPLIST_INDEX                       LIBDB_WRITE_BUFFER_INDEX_LEVELS
#define SKIPLIST_RESERVE_LEVEL               LIBDB_WRITE_BUFFER_INDEX_RESERVE_LEVELS
#define BLOCK_CHOOSE_PERCENT                 LIBDB_GC_BLOCK_CHOOSE_PERCENT
#define MAX_PENDING_SCANS                    LIBDB_MAX_PENDING_SCANS
#define MAX_THREAD                           LIBDB_MAX_THREADS

#endif
