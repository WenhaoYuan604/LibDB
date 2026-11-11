#ifndef SUBSPACE_H_
#define SUBSPACE_H_

#include "./base.h"
#include "./pmdk.h"

struct write_buffer_index;
struct kvcache;
struct subspace_info{
    key_type boundary;
    uint32_t key_num;
    uint32_t slot_num;
    uint32_t flush_block_num;
    uint32_t total_block_num;
    bool is_gc;
    bool is_spliting;
    bool is_merging;
    uint32_t read_refs;
    struct subspace_info *redirect;

    uint64_t redundant_size;
    uint64_t buffered_size;

    uint8_t slot_key_num[SUBSPACE_MAX_SLOT];
    key_type min_key[SUBSPACE_MAX_SLOT];

    PMEMoid slot[SUBSPACE_MAX_SLOT];
    bool need_persist[SUBSPACE_MAX_SLOT];
    PMEMobjpool * pool[SUBSPACE_MAX_SLOT];

    struct nvm_index_slot **migration_slot_shadow;

    struct write_buffer_index * buffer_index;
    struct kvcache * kvcache;
};

#include "./write_buffer_index.h"
#include "kvcache.h"

static inline struct nvm_index_slot *subspace_nvm_slot(
    struct subspace_info *subspace_info, uint32_t slot_index){
    if(slot_index >= subspace_info->slot_num) return NULL;
    if(subspace_info->migration_slot_shadow != NULL &&
       subspace_info->migration_slot_shadow[slot_index] != NULL){
        return subspace_info->migration_slot_shadow[slot_index];
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

uint32_t subspace_live_keys(struct subspace_info *group);
void subspace_ref(struct subspace_info *group);
void subspace_unref(struct subspace_info *group);
bool subspace_busy(const struct subspace_info *group);

void subspace_info_output( struct subspace_info * subspace_info );
struct subspace_info * subspace_info_initialize();
void subspace_info_output_detail( struct subspace_info * subspace_info );
void subspace_info_output_all();

#endif
