#ifndef EE65D866_82E5_4791_9C1A_039527E7D30A
#define EE65D866_82E5_4791_9C1A_039527E7D30A

#include "./kvbuffer.h"
#include "./index.h"
#include "./merge_flush.h"
#include "./spdk_struct.h"
#include "./garbadge_c.h"
#include "base.h"
#include "./kvcache.h"
#include "spdk/env.h"

#include "space_stats.h"

struct db{
    struct initial_load_stats initial_load;
    struct gc_space_control gc_space;
    struct nvm_flush_control nvm_flush;
    struct thread *workers;
    bool phase_io_stats;
    bool load_only;
    char precondition_a[LIBDB_PATH_CAPACITY];
    const char *phase_name;
    bool wal_enabled;
    unsigned wal_mb_per_worker;
    unsigned wal_wait_us;
    unsigned wal_max_inflight;
    uint64_t data_blocks, wal_worker_blocks;
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
    struct kvcache * kvcache;
    struct spdk_ring ** spdk_ring;

    struct spdk_ring ** spdk_result_ring;

    struct server_ring ** ring;
    struct result_ring ** result_ring;
    int server_ring_size;

    int recovery_mode;

    int recovery_dupcheck;
    bool recovery_scan_full;

};

key_type ** subspace_split_initialize(int thread , int size);

int key_to_thread(key_type key , struct db * db);

struct thread * initialize(struct db * db);


struct subspace_info *subspace_split(struct subspace_info *, struct thread *, key_type *);

struct thread * initialize_recovery(struct db * db);


void insert_version(key_type, int, char *, struct thread *, struct insert_time_breakdown *, uint64_t, struct wal_dependency *);

bool subspace_maybe_flush(struct subspace_info *group, struct thread *thread);

bool subspace_force_flush(struct subspace_info *group, struct thread *thread);

void insert(key_type key , int size , char * value ,  struct thread * thread , struct insert_time_breakdown * break_down);

#endif
