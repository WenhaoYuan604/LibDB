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


// PMEMoid subspace_write_buffer_search(struct subspace_info * subspace_info ,key_type key){
//     PMEMoid id;
//     return id;
// }


// // struct subspace * initialize_subspace(int num_subspace){
// //     struct subspace * subspace = (struct subspace *)malloc(sizeof(struct subspace));
// //     int buffer_size = num_subspace * (4800 + sizeof(struct subspace_buffer));
// //     PMEMoid root;

// //     if(buffer_size < PMEMOBJ_MIN_POOL)
// //         buffer_size = PMEMOBJ_MIN_POOL;

// //     subspace->subspace_curr_block = (int *)malloc(sizeof(int)*num_subspace);
// //     subspace->buffer = pmemobj_open("<pmem-dir>/buffer0.pool","buffer0");
// //     if(!subspace->buffer){
// //         subspace->buffer = pmemobj_create("<pmem-dir>/buffer0.pool","buffer0",buffer_size,0666);
// //         if(subspace->buffer == NULL){
// //             perror("pmemobj_create failed\n");
// //             return 0;
// //         }
// //     }

// //     if(!pm_subspace_alloc_buffer(num_subspace,subspace->buffer)){
// //         printf("failed to alloc buffer for subspace\n");
// //         return NULL;
// //     }
// //     return subspace;
// // }

// // int pm_subspace_alloc_buffer(int num_subspace,PMEMobjpool * buffer){
// //     struct subspace_buffer * head_for_block;

// //     PMEMoid root = pmemobj_root(buffer,sizeof(struct subspace_buffer)*num_subspace);
// //     if(OID_IS_NULL(root)){
// //         printf("Couldn't access root obj\n");
// //         return 0;
// //     }

// //     head_for_block = (struct subspace_buffer * )pmemobj_direct(root);
// //     for(int i = 0; i < num_subspace ; i++ ){
// //         // printf("i is %d\n",i);
// //         if(OID_IS_NULL(head_for_block -> id)){
// //             // printf("i is %d\n",i);
// //             if(!pmemobj_alloc(buffer,&(head_for_block->id),4096,0,0,0)){
// //                 head_for_block->off = 0;
// //                 pmemobj_persist(buffer,&(head_for_block->off),sizeof(short));
// //                 // printf("$");
// //             }
// //             else{
// //                 printf("failed to alloc buffer for subspace %d\n",i);
// //                 return 0;
// //             }
// //         }
// //         head_for_block++ ;
// //     }
// //     return 1;
// // }




// struct subspace * initialize_subspace(int num_subspace){
//     struct subspace * subspace = (struct subspace *)malloc(sizeof(struct subspace));
//     int buffer_size = num_subspace * (sizeof(struct subspace_buffer) + 1024);
//     PMEMoid root;

//     if(buffer_size < PMEMOBJ_MIN_POOL)
//         buffer_size = PMEMOBJ_MIN_POOL;

//     subspace->subspace_total = num_subspace;
//     subspace->inf = (struct inf_for_subspace *)malloc(sizeof(struct inf_for_subspace)*num_subspace);
//     // for(int i = 0 ; i < num_subspace ; i++ ){
//     //     subspace->inf[i].curr_block = get_free_block(subspace->db->ns_entry , 0);//
//     //     subspace->inf[i].used = 0;
//     // }
//     // memset(subspace->inf , 0 , sizeof(struct inf_for_subspace) * num_subspace);

//     subspace->buffer = pmemobj_open("<pmem-dir>/buffer0.pool","buffer0");
//     if(!subspace->buffer){
//         subspace->buffer = pmemobj_create("<pmem-dir>/buffer0.pool","buffer0",buffer_size,0666);
//         if(subspace->buffer == NULL){
//             perror("pmemobj_create failed\n");
//             return 0;
//         }
//     }

//     // root = pmemobj_root(subspace->buffer,sizeof(struct subspace_buffer) * num_subspace);
//     root = pmemobj_root(subspace->buffer,sizeof(struct subspace_buffer) * num_subspace);
//     if(OID_IS_NULL(root)){
//         printf("Couldn't access root obj\n");
//         return 0;
//     }

//     return subspace;
// }


// struct subspace_buffer * pm_get_subspace_buffer(int subspace_seq , struct subspace * subspace){

//     PMEMoid root = pmemobj_root(subspace->buffer,sizeof(struct subspace_buffer) * subspace->subspace_total);
//     if(OID_IS_NULL(root)){
//         printf("Couldn't access root obj\n");
//         return 0;
//     }

//     struct subspace_buffer * buffer = (struct subspace_buffer * )pmemobj_direct(root);
//     buffer += subspace_seq ;
//     return buffer;
// }

// int get_subspace_seq(key_type key , int subspace_size){
//     return key/subspace_size;
// }
