#ifndef BE5373DA_B385_4850_878C_9EF55D2B7C31
#define BE5373DA_B385_4850_878C_9EF55D2B7C31
#include "kvcache.h"
#ifndef INDEX_H_
#define INDEX_H_

#include "./dram_index.h"

#define ADDR_SET_NULL(o) ((o)->size = 0)

struct thread{
    struct db * db;
    struct lru_list * lru_list;
    uint64_t kv_version_next;
    uint64_t kv_version_limit;
    int thread;
    int core;

    struct spdk_ring * spdk_ring;
    struct spdk_ring ** spdk_result_ring;
    struct segment * curr_segment;
    struct free_space * fs;

    struct hybrid_index * index;
    struct ns_entry * ns_entry;
    struct nvm_index_free_node * index_free_head;

    struct write_buffer * kvbuffer;
    struct write_buffer_free_node * buffer_free_head;
};

struct load_injectors{
    struct db * db;
    int thread;
    int core;
    int nb_load_injectors;
    struct spdk_ring ** spdk_ring;
    struct spdk_ring * spdk_result_ring;

    struct user_req * load_array;
    int load_num;
};

struct hybrid_index{
    struct dram_index * dram_index;
    struct nvm_index * nvm_index;
};

struct hybrid_index * hybrid_index_initialize(key_type ** subspace_array , int thread,  int size);
struct hybrid_index * hybrid_index_initialize_empty_with_nvm(int thread, struct nvm_index * nvm_index);

struct subspace_info * hybrid_index_insert(struct hybrid_index * hybrid_index, key_type key , int size, PMEMoid id ,
 uint16_t WBF_seq , struct write_buffer_return_type * buffer_to_free , int thread);

int hybrid_index_search(struct hybrid_index * hybrid_index, key_type key , struct write_buffer_index_node *  node_out , struct address_type * addr_out);

#endif

#endif /* BE5373DA_B385_4850_878C_9EF55D2B7C31 */
