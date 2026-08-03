#include "./index.h"
#include "dram_index.h"
#include "write_buffer_index.h"

struct hybrid_index * hybrid_index_initialize(key_type ** subspace_array , int thread,  int size){
    struct hybrid_index * hybrid_index = (struct hybrid_index *)malloc(sizeof(struct hybrid_index) * thread);
    int i;
    printf("dram_index_initialize start\n");
    for(i = 0 ; i < thread ; i++){
        hybrid_index[i].dram_index = dram_index_initialize(subspace_array[i] , size);
    }
    printf("sizeof(struct subspace_info) is %zu\n" , sizeof(struct subspace_info));
    printf("nvm_index_initialize start\n");
    hybrid_index[0].nvm_index = nvm_index_initialize();
    printf("nvm_index_initialize done\n");
    i = 1;
    while( i < thread){
        hybrid_index[i].nvm_index = hybrid_index[0].nvm_index;
        i++;
    }

    return hybrid_index;
}

struct hybrid_index * hybrid_index_initialize_empty_with_nvm(int thread, struct nvm_index * nvm_index){
    struct hybrid_index * hybrid_index = (struct hybrid_index *)malloc(sizeof(struct hybrid_index) * thread);
    if(hybrid_index == NULL){
        return NULL;
    }

    for(int i = 0 ; i < thread ; i++){
        hybrid_index[i].dram_index = dram_index_initialize(NULL , 0);
        hybrid_index[i].nvm_index = nvm_index;
    }
    return hybrid_index;
}

struct subspace_info * hybrid_index_insert(struct hybrid_index * hybrid_index, key_type key , int size, PMEMoid id ,
 uint16_t WBF_seq , struct write_buffer_return_type * buffer_to_free , int thread){
    struct subspace_info * subspace_info;
    struct dram_index_node * dram_node_p;
    subspace_info = dram_index_search(hybrid_index->dram_index , key , &dram_node_p , NULL);
    *buffer_to_free = nvm_index_insert(subspace_info , id , key , size , WBF_seq , thread);
    return subspace_info;
}

int hybrid_index_search(struct hybrid_index * hybrid_index, key_type key , struct write_buffer_index_node *  node_out , struct address_type * addr_out){
    struct subspace_info * subspace_info;
    struct dram_index_node * dram_node_p;
    struct write_buffer_index_node * buffer_index_node;
    struct write_buffer_index * index;
    struct address_type addr;
    subspace_info = dram_index_search(hybrid_index->dram_index , key , &dram_node_p , NULL);
    index = subspace_info->buffer_index;
    while(index){
        buffer_index_node = write_buffer_index_search(index , key);
        if(buffer_index_node != NULL){
            break;
        }
        else{
            index = index->next;
        }
    }

    if(buffer_index_node == NULL){//不存在于write buffer
        addr = nvm_index_search(subspace_info , key);
        if(ADDR_IS_NULL(addr)){
            return 0;
        }
        else{
            *addr_out = addr;
            return 1;
        }
    }
    else{//存在于subspace_info
        *node_out = *buffer_index_node;
        return 2;
    }
}

void subspace_info_output_all(struct dram_index * dram_index){

}
