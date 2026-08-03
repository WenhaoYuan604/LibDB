#ifndef SUBSPACE_H_
#define SUBSPACE_H_

#include "./base.h"
#include "./pmdk.h"
/* 先完成 subspace_info，再拉 write_buffer_index / kvcache，避免与 nvm_index.h 等组成循环包含 */
struct write_buffer_index;
struct kvcache;
// #include "./spdk_struct.h"
// #include "./db.h"
// #include "./kvbuffer.h"
struct subspace_info{
    key_type boundary;//该 group 在 DRAM index 中的边界
    uint32_t key_num;//该subspace的键值对个数
    uint32_t slot_num;//该subspace的slot个数
    uint32_t flush_block_num;
    uint32_t total_block_num;
    // uint32_t * block_array;
    bool is_gc;
    bool is_spliting;

    uint16_t redundant_size;//冗余数据size
    uint32_t buffered_size;//被缓存的数据size

    uint8_t slot_key_num[SUBSPACE_MAX_SLOT];//每个slot内的键值对个数
    key_type min_key[SUBSPACE_MAX_SLOT];//每个slot最小键

    PMEMoid slot[SUBSPACE_MAX_SLOT];//每个slot的地址
    bool need_persist[SUBSPACE_MAX_SLOT];
    PMEMobjpool * pool[SUBSPACE_MAX_SLOT];//每个slot的pool
    /* split 未提交期间使用的 DRAM-only slot table；NULL 时读取 PMEM。 */
    struct nvm_index_slot **split_slot_shadow;

    struct write_buffer_index * buffer_index;
    struct kvcache * kvcache;
};

#include "./write_buffer_index.h"
#include "kvcache.h"

static inline struct nvm_index_slot *subspace_nvm_slot(
    struct subspace_info *subspace_info, uint32_t slot_index){
    if(subspace_info->split_slot_shadow != NULL &&
       subspace_info->split_slot_shadow[slot_index] != NULL){
        return subspace_info->split_slot_shadow[slot_index];
    }
    return (struct nvm_index_slot *)pmemobj_direct(
        subspace_info->slot[slot_index]);
}

static inline struct slot_element *subspace_slot_elements(
    struct subspace_info *subspace_info, uint32_t slot_index){
    struct nvm_index_slot *slot = subspace_nvm_slot(subspace_info, slot_index);
    return slot == NULL ? NULL : slot->entries;
}

static inline key_type subspace_slot_boundary(
    struct subspace_info *subspace_info, uint32_t slot_index){
    struct nvm_index_slot *slot = subspace_nvm_slot(subspace_info, slot_index);
    return slot == NULL ? KEY_MAX : slot->boundary;
}

void subspace_info_output( struct subspace_info * subspace_info );

struct subspace_info * subspace_info_initialize();

void subspace_info_output_detail( struct subspace_info * subspace_info );

void subspace_info_output_all();
#endif














// struct root_for_subspace{
//     PMEMoid id;
//     short off;
// };

// struct subspace{
//     PMEMobjpool * buffer;
//     int subspace_size;
//     int subspace_total;
//     struct inf_for_subspace * inf;
//     struct ns_entry * ns_entry;
//     struct free_space * fs;
//     struct segment ** cur_segment;
// };

// struct subspace_buffer{
//     char buffer[4096];
// };

// struct inf_for_subspace{
//     int used;//该subspace的buffer已占用空间大小
//     int curr_block;//该subspace
//     int total_size;//
//     int valid_size;//
// };

// int get_subspace_seq(key_type key , int subapce_size);

// struct subspace * initialize_subspace(int num_subspace);

// // int pm_subspace_alloc_buffer(int num_subspace , PMEMobjpool * buffer);

// struct subspace_buffer * pm_get_subspace_buffer(int subspace_seq , struct subspace * subspace);

// // char * read_from_subspace(int subspace_seq , struct subspace * subspace);
