#ifndef EE65D866_82E5_4791_9C1A_039527E7D30A
#define EE65D866_82E5_4791_9C1A_039527E7D30A

#include "./kvbuffer.h"
#include "./index.h"
#include "./leaf_node_file.h"
// #include "subspace.h"
#include <libpmemobj/base.h>
#include "./merge_flush.h"
#include "./spdk_struct.h"
#include "./garbadge_c.h"
#include "base.h"
#include "./kvcache.h"
#include "spdk/env.h"

struct db{
    int thread;
    int load_injectors;
    int boundary_size;
    int tasks_per_thread;
    int size_kv;
    int size_value;
    uint64_t cachesize;
    FILE * output_fp;
    key_type ** boundary;
    struct free_space * fs;
    struct ns_entry * ns_entry;
    struct hybrid_index * index;
    struct write_buffer * write_buffer;
    struct kvcache * kvcache;
    struct spdk_ring ** spdk_ring;

    struct spdk_ring ** spdk_result_ring;

    struct server_ring ** ring;
    struct result_ring ** result_ring;
    int server_ring_size;
    /** 非 0：命令行指定从 NVM 重建 dram_index / subspace（见 initialize_recovery） */
    int recovery_mode;
    /** recovery 完成后是否做 NVM 全量键排序查重（费内存/时间，见 --recovery-dupcheck） */
    int recovery_dupcheck;
    /**
     * recovery 扫描时曾带 LIBDB_INVALID_GROUP_BOUNDARY marker 的 slots。
     * 修复完成后仍保留其持久化地址，用于诊断与核对 crash-recovery 路径。
     */
    PMEMoid *recovery_marked_slots;
    uint32_t recovery_marked_slot_count;
    struct nvm_index_free_node **recovery_index_free_heads;
};

key_type ** subspace_split_initialize(int thread , int size);

int key_to_thread(key_type key , struct db * db);

struct thread * initialize(struct db * db);

struct thread * initialize_recovery(struct db * db);


void insert(key_type key , int size , char * value ,  struct thread * thread , struct insert_time_breakdown * break_down);

#endif /* EE65D866_82E5_4791_9C1A_039527E7D30A */
