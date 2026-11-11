#ifndef EE65D866_82E5_4791_9C1A_039527E7D30A
#define EE65D866_82E5_4791_9C1A_039527E7D30A

#include "./kvbuffer.h"
#include "./index.h"
#include "./leaf_node_file.h"
#include <libpmemobj/base.h>
#include "./merge_flush.h"
#include "./spdk_struct.h"
#include "./garbadge_c.h"
#include "base.h"
#include "space_stats.h"
#include "./kvcache.h"
#include "spdk/env.h"

struct db{
    const char *benchmark_phase;
    struct initial_load_stats initial_load;
    struct gc_space_control gc_space;
    struct nvm_flush_control nvm_flush;
    uint64_t data_blocks;
    struct thread *workers;
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

    int recovery_mode;

    int recovery_dupcheck;




    PMEMoid *recovery_marked_slots;
    uint32_t recovery_marked_slot_count;
    struct nvm_index_free_node **recovery_index_free_heads;
};

key_type ** subspace_split_initialize(int thread , int size);
struct subspace_info *subspace_split(struct subspace_info *group,
                                    struct thread *thread, key_type *boundary);

int key_to_thread(key_type key , struct db * db);

struct thread * initialize(struct db * db);

struct thread * initialize_recovery(struct db * db);



bool subspace_maybe_flush(struct subspace_info *group, struct thread *thread);

void insert(key_type key , int size , char * value ,  struct thread * thread , struct insert_time_breakdown * break_down);

#endif
