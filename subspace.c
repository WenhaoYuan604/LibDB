#include "./subspace.h"
#include "nvm_index.h"
#include "write_buffer_index.h"

struct subspace_info * subspace_info_initialize(){
    struct subspace_info * subspace_info = (struct subspace_info *)malloc(sizeof(struct subspace_info));
    memset(subspace_info , 0 , sizeof(struct subspace_info));
    subspace_info->buffer_index = (struct write_buffer_index *)malloc(sizeof(struct write_buffer_index));
    write_buffer_index_init(subspace_info->buffer_index);
    subspace_info->kvcache = kvcache_init();
    return subspace_info;
}

void subspace_info_output( struct subspace_info * subspace_info ){
    printf("subspace key num:%d\nslot_num:%d\n",
           subspace_info->key_num, subspace_info->slot_num);
    for(int i = 0; i < subspace_info->slot_num; i++){
        struct nvm_index_slot *slot = subspace_nvm_slot(subspace_info, (uint32_t)i);
        printf("slot%d:key_num:%d min_key:%lu boundary:%lu slot:%p\n",
               i, subspace_info->slot_key_num[i], subspace_info->min_key[i],
               slot == NULL ? KEY_MAX : slot->boundary, (void *)slot);
    }
    return ;
}

void subspace_info_output_detail( struct subspace_info * subspace_info ){
    printf("subspace key num:%d\nslot_num:%d\n",
           subspace_info->key_num, subspace_info->slot_num);
    for(int i = 0; i < subspace_info->slot_num; i++){
        struct nvm_index_slot *slot = subspace_nvm_slot(subspace_info, (uint32_t)i);
        if(slot == NULL){
            continue;
        }
        printf("slot%d:key_num:%d min_key:%lu boundary:%lu slot:%p\n",
               i, subspace_info->slot_key_num[i], subspace_info->min_key[i],
               slot->boundary, (void *)slot);
        for(int j = 0; j < subspace_info->slot_key_num[i]; j++){
            struct slot_element *element = &slot->entries[j];
            printf("key:%lu addr:block%d off %d size %d\n", element->key,
                   element->addr.block, element->addr.off, element->addr.size);
        }
        printf("\n");
    }
    return ;
}

bool subspace_busy(const struct subspace_info *group){
    return group->is_merging || group->is_spliting || group->is_gc || group->flush_block_num;
}

/* Count the union, not the sum: active GWB shadows frozen GWB and SSD. */
uint32_t subspace_live_keys(struct subspace_info *group){
    uint32_t count = group->key_num;
    for(struct write_buffer_index *index = group->buffer_index; index; index = index->next){
        for(struct write_buffer_index_node *node = index->head->forward[0]; node; node = node->forward[0]){
            bool exists = !ADDR_IS_NULL(nvm_index_search(group, node->key));
            for(struct write_buffer_index *newer = group->buffer_index;
                !exists && newer != index; newer = newer->next)
                exists = write_buffer_index_search(newer, node->key) != NULL;
            if(!exists) count++;
        }
    }
    return count;
}

void subspace_ref(struct subspace_info *group){ group->read_refs++; }

void subspace_unref(struct subspace_info *group){
    assert(group->read_refs);
    group->read_refs--;
    if(!group->read_refs && group->redirect){
        struct subspace_info *successor = group->redirect;
        free(group);
        subspace_unref(successor);
    }
}
