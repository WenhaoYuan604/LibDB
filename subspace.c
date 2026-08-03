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
