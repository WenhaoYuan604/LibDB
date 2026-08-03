#ifndef LIBDB_CONFIG_H_
#define LIBDB_CONFIG_H_

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

/*
 * LibDB compile-time configuration
 * --------------------------------
 * These values define persistent-memory layout, fixed-size structures, or
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
#define LIBDB_TASK_BUFFER_BLOCK_COUNT            256U

/* Flush, subspace, slot, and free-space group layout. */
#define LIBDB_FLUSH_BLOCK_COUNT                 4
#define LIBDB_LNF_SLOT_SIZE                     256U
#define LIBDB_SUBSPACE_MAX_KEYS                 128U
/* A slot stores one boundary key followed by key-to-SSD-address entries. */
#define LIBDB_LNF_SLOT_ENTRY_CAPACITY           \
    ((LIBDB_LNF_SLOT_SIZE - sizeof(key_type)) / sizeof(struct slot_element))
#define LIBDB_LNF_SLOT_PADDING_BYTES             \
    (LIBDB_LNF_SLOT_SIZE - sizeof(key_type) - \
     LIBDB_LNF_SLOT_ENTRY_CAPACITY * sizeof(struct slot_element))
/* Reserve enough whole slots for twice the maximum subspace key count. */
#define LIBDB_SUBSPACE_MAX_SLOTS                \
    (((2U * LIBDB_SUBSPACE_MAX_KEYS) + LIBDB_LNF_SLOT_ENTRY_CAPACITY - 1U) / \
     LIBDB_LNF_SLOT_ENTRY_CAPACITY)
#define LIBDB_SUBSPACE_REDUNDANT_THRESHOLD      0.3
#define LIBDB_FREE_SPACE_GROUP_BLOCKS           1024
/* Reserved lower-fence marker for an interrupted persistent group split. */
#define LIBDB_INVALID_GROUP_BOUNDARY             UINT64_MAX

/* Persistent write-buffer files. */
#define LIBDB_WBF_FILE_SIZE                     (1ULL * 1024 * 1024 * 1024)
#define LIBDB_WBF_SEGMENT_SIZE                  (4U * 1024 * 1024)
#define LIBDB_WBF_SEGMENTS_PER_FILE             256
#define LIBDB_WBF_MIN_SLOT_SIZE                 32U
/* Largest record whose power-of-two WBF allocation fits uint16_t metadata. */
#define LIBDB_WBF_MAX_RECORD_SIZE               32768U

/* PRISM-style PMEM-cache experiment: valid bytes kept for each value. */
#define LIBDB_PMEM_VALUE_PREFIX_SIZE             32U

/*
 * Leaf slots are stored in 512 MiB PMDK pools. A new leaf_node_fileN pool is
 * created when the current pool can no longer allocate a complete segment.
 * Each 256-byte slot update is committed as an independent PMDK transaction.
 */
#define LIBDB_LNF_FILE_SIZE                     (512ULL * 1024 * 1024)
#define LIBDB_LNF_SEGMENT_SIZE                  (4U * 1024 * 1024)
#define LIBDB_LNF_SEGMENTS_PER_FILE             (LIBDB_LNF_FILE_SIZE / LIBDB_LNF_SEGMENT_SIZE)
#define LIBDB_MAX_PMEM_FILES                    512

/* Index, cache, and GC limits. */
#define LIBDB_DRAM_INDEX_FANOUT                 128
#define LIBDB_NVM_INDEX_MAX_NODE                128
#define LIBDB_KVCACHE_MAX_LEVEL                 3
#define LIBDB_WRITE_BUFFER_INDEX_LEVELS         2
#define LIBDB_WRITE_BUFFER_INDEX_RESERVE_LEVELS 2
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
    char pmem_dir[LIBDB_PATH_CAPACITY];
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
void libdb_config_print(FILE *stream);
char *libdb_config_make_pmem_path(const char *stem, unsigned int file_seq);

/*
 * Compatibility aliases for the current codebase. New code should prefer the
 * LIBDB_* names above. Keeping aliases here makes this the only definition site.
 */
#define BLOCK_SIZE                           LIBDB_BLOCK_SIZE
#define physical_block_to_logical_block      LIBDB_PHYSICAL_BLOCKS_PER_LOGICAL_BLOCK
#define ctr_max_qp                           LIBDB_MAX_NVME_QPAIRS
#define Flush_Blcok_Num                      LIBDB_FLUSH_BLOCK_COUNT
#define SUBSPACE_MAX_KEY                     LIBDB_SUBSPACE_MAX_KEYS
#define LNF_SLOT_MIN_SIZE                    LIBDB_LNF_SLOT_SIZE
#define SUBSPACE_MAX_SLOT                    LIBDB_SUBSPACE_MAX_SLOTS
#define SUBSPACE_REDUNDANT_PERCENT_THRESHOLD LIBDB_SUBSPACE_REDUNDANT_THRESHOLD
#define alloc_grain                          LIBDB_FREE_SPACE_GROUP_BLOCKS
#define WBF_SIZE                             LIBDB_WBF_FILE_SIZE
#define WSB_SIZE                             LIBDB_WBF_SEGMENT_SIZE
#define WSB_NUM_MAX                          LIBDB_WBF_SEGMENTS_PER_FILE
#define WBF_SLOT_MIN_SIZE                    LIBDB_WBF_MIN_SLOT_SIZE
#define LNF_SIZE                             LIBDB_LNF_FILE_SIZE
#define LNS_SIZE                             LIBDB_LNF_SEGMENT_SIZE
#define LNS_NUM_MAX                          LIBDB_LNF_SEGMENTS_PER_FILE
#define MAX_FILE_NUM                         LIBDB_MAX_PMEM_FILES
#define DRAM_INDEX_MAX_NODE                  LIBDB_DRAM_INDEX_FANOUT
#define NVM_INDEX_MAX_NODE                   LIBDB_NVM_INDEX_MAX_NODE
#define MAX_LEVEL                            LIBDB_KVCACHE_MAX_LEVEL
#define SKIPLIST_INDEX                       LIBDB_WRITE_BUFFER_INDEX_LEVELS
#define SKIPLIST_RESERVE_LEVEL               LIBDB_WRITE_BUFFER_INDEX_RESERVE_LEVELS
#define BLOCK_CHOOSE_PERCENT                 LIBDB_GC_BLOCK_CHOOSE_PERCENT
#define MAX_PENDING_SCANS                    LIBDB_MAX_PENDING_SCANS
#define MAX_THREAD                           LIBDB_MAX_THREADS

#endif
